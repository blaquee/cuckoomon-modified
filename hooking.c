/*
Cuckoo Sandbox - Automated Malware Analysis
Copyright (C) 2010-2014 Cuckoo Sandbox Developers

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stddef.h>
#include "ntapi.h"
#include "hooking.h"
#include "hooks.h"
#include "ignore.h"
#include "unhook.h"
#include "misc.h"
#include "pipe.h"

extern DWORD g_tls_hook_index;

#ifdef _WIN64
#define TLS_LAST_WIN32_ERROR 0x68
#define TLS_LAST_NTSTATUS_ERROR 0x1250
#else
#define TLS_LAST_WIN32_ERROR 0x34
#define TLS_LAST_NTSTATUS_ERROR 0xbf4
#endif

void emit_rel(unsigned char *buf, unsigned char *source, unsigned char *target)
{
	*(DWORD *)buf = (DWORD)(target - (source + 4));
}

// need to be very careful about what we call in here, as it can be called in the context of any hook
// including those that hold the loader lock

static int set_caller_info(ULONG_PTR addr)
{
	hook_info_t *hookinfo = hook_info();

	if (!is_in_dll_range(addr)) {
		if (hookinfo->main_caller_retaddr == 0)
			hookinfo->main_caller_retaddr = addr;
		else {
			hookinfo->parent_caller_retaddr = addr;
			return 1;
		}
	}
	return 0;
}

int addr_in_our_dll_range(ULONG_PTR addr)
{
	if (addr >= g_our_dll_base && addr < (g_our_dll_base + g_our_dll_size))
		return 1;
	return 0;
}

int called_by_hook(void)
{
	hook_info_t *hookinfo = hook_info();

	return operate_on_backtrace(hookinfo->return_address, hookinfo->frame_pointer, addr_in_our_dll_range);
}

extern BOOLEAN is_ignored_thread(DWORD tid);
extern CRITICAL_SECTION g_tmp_hookinfo_lock;

static hook_info_t tmphookinfo;
DWORD tmphookinfo_threadid;

// returns 1 if we should call our hook, 0 if we should call the original function instead
// on x86 this is actually: hook, esp, ebp
// on x64 this is actually: hook, rsp, rip of hook (for unwind-based stack walking)
int WINAPI enter_hook(hook_t *h, ULONG_PTR sp, ULONG_PTR ebp_or_rip)
{
	hook_info_t *hookinfo;
	
	if (g_tls_hook_index >= 0x40 && h->new_func == &New_NtAllocateVirtualMemory) {
		lasterror_t lasterrors;
		get_lasterrors(&lasterrors);
		if (TlsGetValue(g_tls_hook_index) == NULL && (!tmphookinfo_threadid || tmphookinfo_threadid != GetCurrentThreadId())) {
			EnterCriticalSection(&g_tmp_hookinfo_lock);
			memset(&tmphookinfo, 0, sizeof(tmphookinfo));
			tmphookinfo_threadid = GetCurrentThreadId();
		}
		set_lasterrors(&lasterrors);
	}
	else if (tmphookinfo_threadid) {
		tmphookinfo_threadid = 0;
		LeaveCriticalSection(&g_tmp_hookinfo_lock);
	}

	hookinfo = hook_info();

	hookinfo->current_hook = h;
	hookinfo->stack_pointer = sp;
	hookinfo->return_address = *(ULONG_PTR *)sp;
	hookinfo->frame_pointer = ebp_or_rip;

	if ((hookinfo->disable_count < 1) && (h->allow_hook_recursion || (!called_by_hook() /*&& !is_ignored_thread(GetCurrentThreadId())*/))) {
		/* set caller information */
		hookinfo->main_caller_retaddr = 0;
		hookinfo->parent_caller_retaddr = 0;

		operate_on_backtrace(sp, ebp_or_rip, set_caller_info);

		return 1;
	}

	return 0;
}

hook_info_t *hook_info()
{
	hook_info_t *ptr;

	lasterror_t lasterror;
	
	if (tmphookinfo_threadid && tmphookinfo_threadid == GetCurrentThreadId())
		return &tmphookinfo;

	get_lasterrors(&lasterror);

	ptr = (hook_info_t *)TlsGetValue(g_tls_hook_index);
	if (ptr == NULL) {
		ptr = (hook_info_t *)calloc(1, sizeof(hook_info_t));
		TlsSetValue(g_tls_hook_index, ptr);
	}

	set_lasterrors(&lasterror);

	return ptr;
}

void get_lasterrors(lasterror_t *errors)
{
	char *teb = (char *)NtCurrentTeb();

	errors->Win32Error = *(DWORD *)(teb + TLS_LAST_WIN32_ERROR);
	errors->NtstatusError = *(DWORD *)(teb + TLS_LAST_NTSTATUS_ERROR);
}

// we do our own version of this function to avoid the potential debug triggers
void set_lasterrors(lasterror_t *errors)
{
	char *teb = (char *)NtCurrentTeb();

	*(DWORD *)(teb + TLS_LAST_WIN32_ERROR) = errors->Win32Error;
	*(DWORD *)(teb + TLS_LAST_NTSTATUS_ERROR) = errors->NtstatusError;
}

void hook_enable()
{
    hook_info()->disable_count--;
}

void hook_disable()
{
    hook_info()->disable_count++;
}
