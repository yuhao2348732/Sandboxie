/*
 * Copyright 2004-2020 Sandboxie Holdings, LLC 
 * Copyright 2020-2021 David Xanatos, xanasoft.com
 *
 * This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

//---------------------------------------------------------------------------
// Session Management
//---------------------------------------------------------------------------


#include "session.h"
#include "util.h"
#include "conf.h"
#include "api.h"
#include "process.h"
#include "obj.h"
#include "log_buff.h"


//---------------------------------------------------------------------------
// Defines
//---------------------------------------------------------------------------


#define SESSION_MONITOR_BUF_SIZE    (PAGE_SIZE * 32)


//---------------------------------------------------------------------------
// Structures and Types
//---------------------------------------------------------------------------


struct _SESSION {

    // changes to the linked list of SESSION blocks are synchronized by
    // an exclusive lock on Session_ListLock

    LIST_ELEM list_elem;

    //
    // session id
    //

    ULONG session_id;

    //
    // session leader process id
    //

    HANDLE leader_pid;

    //
    // disable forced process
    //

    LONGLONG disable_force_time;

    //
    // resource monitor
    //

	LOG_BUFFER* monitor_log;

};


typedef struct _SESSION             SESSION;


//---------------------------------------------------------------------------
// Functions
//---------------------------------------------------------------------------


static BOOLEAN Session_AddObjectType(const WCHAR *TypeName);

static void Session_Unlock(KIRQL irql);

static SESSION *Session_Get(
    BOOLEAN create, ULONG SessionId, KIRQL *out_irql);

static BOOLEAN Session_CheckAdminAccess(const WCHAR *setting);


//---------------------------------------------------------------------------


static NTSTATUS Session_Api_Leader(PROCESS *proc, ULONG64 *parms);

static NTSTATUS Session_Api_DisableForce(PROCESS *proc, ULONG64 *parms);

static NTSTATUS Session_Api_MonitorControl(PROCESS *proc, ULONG64 *parms);

//static NTSTATUS Session_Api_MonitorPut(PROCESS *proc, ULONG64 *parms);

static NTSTATUS Session_Api_MonitorPut2(PROCESS *proc, ULONG64 *parms);

//static NTSTATUS Session_Api_MonitorGet(PROCESS *proc, ULONG64 *parms);

static NTSTATUS Session_Api_MonitorGetEx(PROCESS *proc, ULONG64 *parms);


//---------------------------------------------------------------------------


#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, Session_AddObjectType)
#endif // ALLOC_PRAGMA


//---------------------------------------------------------------------------
// Variables
//---------------------------------------------------------------------------


static LIST Session_List;
PERESOURCE Session_ListLock = NULL;

volatile LONG Session_MonitorCount = 0;

static POBJECT_TYPE *Session_ObjectTypes = NULL;


//---------------------------------------------------------------------------
// Session_Init
//---------------------------------------------------------------------------


_FX BOOLEAN Session_Init(void)
{
    List_Init(&Session_List);

    if (! Mem_GetLockResource(&Session_ListLock, TRUE))
        return FALSE;

    Api_SetFunction(API_SESSION_LEADER,         Session_Api_Leader);
    Api_SetFunction(API_DISABLE_FORCE_PROCESS,  Session_Api_DisableForce);
    Api_SetFunction(API_MONITOR_CONTROL,        Session_Api_MonitorControl);
    //Api_SetFunction(API_MONITOR_PUT,            Session_Api_MonitorPut);
    Api_SetFunction(API_MONITOR_PUT2,           Session_Api_MonitorPut2);
    //Api_SetFunction(API_MONITOR_GET,            Session_Api_MonitorGet);
	Api_SetFunction(API_MONITOR_GET_EX,			Session_Api_MonitorGetEx);

    //
    // initialize set of recognized objects types for Session_Api_MonitorPut
    //

    Session_ObjectTypes = Mem_AllocEx(
                            Driver_Pool, sizeof(POBJECT_TYPE) * 9, TRUE);
    if (! Session_ObjectTypes)
        return FALSE;
    memzero(Session_ObjectTypes, sizeof(POBJECT_TYPE) * 9);

    if (! Session_AddObjectType(L"Job"))
        return FALSE;
    if (! Session_AddObjectType(L"Event"))
        return FALSE;
    if (! Session_AddObjectType(L"Mutant"))
        return FALSE;
    if (! Session_AddObjectType(L"Semaphore"))
        return FALSE;
    if (! Session_AddObjectType(L"Section"))
        return FALSE;
    if (Driver_OsVersion < DRIVER_WINDOWS_VISTA) {
        if (! Session_AddObjectType(L"Port"))
            return FALSE;
    } else {
        if (! Session_AddObjectType(L"ALPC Port"))
            return FALSE;
    }

    return TRUE;
}


//---------------------------------------------------------------------------
// Session_Unload
//---------------------------------------------------------------------------


_FX void Session_Unload(void)
{
    if (Session_ListLock) {

        Session_Cancel(NULL);
        Mem_FreeLockResource(&Session_ListLock);
    }
}


//---------------------------------------------------------------------------
// Session_AddObjectType
//---------------------------------------------------------------------------


_FX BOOLEAN Session_AddObjectType(const WCHAR *TypeName)
{
    NTSTATUS status;
    WCHAR ObjectName[64];
    UNICODE_STRING uni;
    OBJECT_ATTRIBUTES objattrs;
    HANDLE handle;
    OBJECT_TYPE *object;
    ULONG i;

    wcscpy(ObjectName, L"\\ObjectTypes\\");
    wcscat(ObjectName, TypeName);
    RtlInitUnicodeString(&uni, ObjectName);
    InitializeObjectAttributes(&objattrs,
        &uni, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    //
    // Windows 7 requires that we pass ObjectType in the second parameter
    // below, while earlier versions of Windows do not require this.
    // Obj_GetTypeObjectType() returns ObjectType on Windows 7, and
    // NULL on earlier versions of Windows
    //

    status = ObOpenObjectByName(
                    &objattrs, Obj_GetTypeObjectType(), KernelMode,
                    NULL, 0, NULL, &handle);
    if (! NT_SUCCESS(status)) {
        Log_Status_Ex(MSG_OBJ_HOOK_ANY_PROC, 0x44, status, TypeName);
        return FALSE;
    }

    status = ObReferenceObjectByHandle(
                    handle, 0, NULL, KernelMode, &object, NULL);

    ZwClose(handle);

    if (! NT_SUCCESS(status)) {
        Log_Status_Ex(MSG_OBJ_HOOK_ANY_PROC, 0x55, status, TypeName);
        return FALSE;
    }

    ObDereferenceObject(object);

    for (i = 0; Session_ObjectTypes[i]; ++i)
        ;
    Session_ObjectTypes[i] = object;

    return TRUE;
}


//---------------------------------------------------------------------------
// Session_Unlock
//---------------------------------------------------------------------------


_FX void Session_Unlock(KIRQL irql)
{
    ExReleaseResourceLite(Session_ListLock);
    KeLowerIrql(irql);
}


//---------------------------------------------------------------------------
// Session_Get
//---------------------------------------------------------------------------


_FX SESSION *Session_Get(BOOLEAN create, ULONG SessionId, KIRQL *out_irql)
{
    NTSTATUS status;
    SESSION *session;

    if (SessionId == -1) {
        status = MyGetSessionId(&SessionId);
        if (! NT_SUCCESS(status))
            return NULL;
    }

    //
    // find an existing SESSION block or create a new one
    //

    KeRaiseIrql(APC_LEVEL, out_irql);
    ExAcquireResourceExclusiveLite(Session_ListLock, TRUE);

    session = List_Head(&Session_List);
    while (session) {
        if (session->session_id == SessionId)
            break;
        session = List_Next(session);
    }

    if ((! session) && create) {

        session = Mem_Alloc(Driver_Pool, sizeof(SESSION));
        if (session) {

            memzero(session, sizeof(SESSION));
            session->session_id = SessionId;

            List_Insert_After(&Session_List, NULL, session);
        }
    }

    if (! session)
        Session_Unlock(*out_irql);
    else if (create)
        session->leader_pid = PsGetCurrentProcessId();

    return session;
}


//---------------------------------------------------------------------------
// Session_Cancel
//---------------------------------------------------------------------------


_FX void Session_Cancel(HANDLE ProcessId)
{
    KIRQL irql;
    SESSION *session;

    //
    // find an existing SESSION block with leader_pid == ProcessId
    //

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(Session_ListLock, TRUE);

    session = List_Head(&Session_List);
    while (session) {
        if ((session->leader_pid == ProcessId) || (! ProcessId)) {

            if (session->monitor_log) {
				log_buffer_free(session->monitor_log);
                InterlockedDecrement(&Session_MonitorCount);
            }

            List_Remove(&Session_List, session);
            Mem_Free(session, sizeof(SESSION));

            break;
        }

        session = List_Next(session);
    }

    Session_Unlock(irql);
}


//---------------------------------------------------------------------------
// Session_CheckAdminAccess
//---------------------------------------------------------------------------


_FX BOOLEAN Session_CheckAdminAccess(const WCHAR *setting)
{
    if (Conf_Get_Boolean(NULL, setting, 0, FALSE)) {

        //
        // check if token is member of the Administrators group
        //

        PACCESS_TOKEN pAccessToken =
            PsReferencePrimaryToken(PsGetCurrentProcess());
        BOOLEAN IsAdmin = SeTokenIsAdmin(pAccessToken);
        if ((! IsAdmin) && Driver_OsVersion >= DRIVER_WINDOWS_VISTA) {

            //
            // on Windows Vista, check for UAC split token
            //

            ULONG *pElevationType;
            NTSTATUS status = SeQueryInformationToken(
                pAccessToken, TokenElevationType, &pElevationType);
            if (NT_SUCCESS(status)) {
                if (*pElevationType == TokenElevationTypeFull ||
                    *pElevationType == TokenElevationTypeLimited)
                    IsAdmin = TRUE;
                ExFreePool(pElevationType);
            }
        }

        PsDereferencePrimaryToken(pAccessToken);
        if (! IsAdmin)
            return FALSE;
    }
    return TRUE;
}


//---------------------------------------------------------------------------
// Session_Api_Leader
//---------------------------------------------------------------------------


_FX NTSTATUS Session_Api_Leader(PROCESS *proc, ULONG64 *parms)
{
    API_SESSION_LEADER_ARGS *args = (API_SESSION_LEADER_ARGS *)parms;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG64 ProcessIdToReturn = 0;
    SESSION *session = NULL;
    KIRQL irql;

    ULONG64 *user_pid = args->process_id.val;
    if (! user_pid) {

        //
        // set leader
        //

        if (proc)
            status = STATUS_NOT_IMPLEMENTED;
        else {

            session = Session_Get(TRUE, -1, &irql);
            if (! session)
                status = STATUS_INSUFFICIENT_RESOURCES;
        }

    } else {

        //
        // get leader
        //

        HANDLE TokenHandle = args->token_handle.val;

        ULONG SessionId;
        ULONG len = sizeof(ULONG);

        status = ZwQueryInformationToken(
                        TokenHandle, TokenSessionId, &SessionId, len, &len);

        if (NT_SUCCESS(status)) {

            __try {

                session = Session_Get(FALSE, SessionId, &irql);
                if (session)
                    ProcessIdToReturn = (ULONG64)session->leader_pid;

            } __except (EXCEPTION_EXECUTE_HANDLER) {
                status = GetExceptionCode();
            }
        }
    }

    if (session)
        Session_Unlock(irql);

    if (user_pid && NT_SUCCESS(status)) {
        ProbeForWrite(user_pid, sizeof(ULONG64), sizeof(ULONG64));
        *user_pid = ProcessIdToReturn;
    }

    return status;
}


//---------------------------------------------------------------------------
// Session_Api_DisableForce
//---------------------------------------------------------------------------


_FX NTSTATUS Session_Api_DisableForce(PROCESS *proc, ULONG64 *parms)
{
    API_DISABLE_FORCE_PROCESS_ARGS *args =
        (API_DISABLE_FORCE_PROCESS_ARGS *)parms;
    ULONG *in_flag;
    ULONG *out_flag;
    LARGE_INTEGER time;
    SESSION *session;
    KIRQL irql;

    if (proc)
        return STATUS_NOT_IMPLEMENTED;

    //
    // get status
    //

    out_flag = args->get_flag.val;
    if (out_flag) {
        ProbeForWrite(out_flag, sizeof(ULONG), sizeof(ULONG));
        *out_flag = Session_IsForceDisabled(-1);
    }

    //
    // set status
    //

    in_flag = args->set_flag.val;
    if (in_flag) {
        ProbeForRead(in_flag, sizeof(ULONG), sizeof(ULONG));
        ULONG in_flag_value = *in_flag;
        if (in_flag_value) {

            if (! Session_CheckAdminAccess(L"ForceDisableAdminOnly"))
                    return STATUS_ACCESS_DENIED;
            KeQuerySystemTime(&time);

        } else
            time.QuadPart = 0;

        if (in_flag_value == DISABLE_JUST_THIS_PROCESS) {

            Process_DfpInsert(PROCESS_TERMINATED, PsGetCurrentProcessId());

        } else {

            session = Session_Get(FALSE, -1, &irql);
            if (session) {
                session->disable_force_time = time.QuadPart;
                Session_Unlock(irql);
            }
        }
    }

    return STATUS_SUCCESS;
}


//---------------------------------------------------------------------------
// Session_IsForceDisabled
//---------------------------------------------------------------------------


_FX BOOLEAN Session_IsForceDisabled(ULONG SessionId)
{
    int seconds;
    LARGE_INTEGER time;
    LONGLONG diff;
    SESSION *session;
    KIRQL irql;

    // compute the number of seconds that force remains disabled

    seconds = Conf_Get_Number(NULL, L"ForceDisableSeconds", 0, 10);

    if (seconds == 0) {
        // zero means never allow to disable force process
        return FALSE;
    }

    // get the number of seconds passed since force was disabled

    KeQuerySystemTime(&time);

    session = Session_Get(FALSE, SessionId, &irql);
    if (! session)
        return FALSE;

    diff = (time.QuadPart - session->disable_force_time) / SECONDS(1);

    Session_Unlock(irql);

    // compare and return

    return (diff <= seconds);
}


//---------------------------------------------------------------------------
// Session_MonitorPut
//---------------------------------------------------------------------------


_FX void Session_MonitorPut(ULONG type, const WCHAR *name, HANDLE pid)
{
	const WCHAR* strings[2] = { name, NULL };
	Session_MonitorPutEx(type, strings, NULL, pid, PsGetCurrentThreadId());
}


//---------------------------------------------------------------------------
// Session_MonitorPutEx
//---------------------------------------------------------------------------


_FX void Session_MonitorPutEx(ULONG type, const WCHAR** strings, ULONG* lengths, HANDLE hpid, HANDLE htid)
{
    SESSION *session;
    KIRQL irql;

    session = Session_Get(FALSE, -1, &irql);
    if (! session)
        return;

    if (session->monitor_log && *strings[0]) {

		ULONG pid = (ULONG)hpid;
        ULONG tid = (ULONG)htid;

		SIZE_T data_len = 0;
		for(int i=0; strings[i] != NULL; i++)
			data_len += (lengths ? lengths [i] : wcslen(strings[i])) * sizeof(WCHAR);

        
		//[Type 4][PID 4][TID 4][Data n*2]
		SIZE_T entry_size = 4 + 4 + 4 + data_len;

		CHAR* write_ptr = log_buffer_push_entry((LOG_BUFFER_SIZE_T)entry_size, session->monitor_log);
		if (write_ptr) {
			log_buffer_push_bytes((CHAR*)&type, 4, &write_ptr, session->monitor_log);
			log_buffer_push_bytes((CHAR*)&pid, 4, &write_ptr, session->monitor_log);
            log_buffer_push_bytes((CHAR*)&tid, 4, &write_ptr, session->monitor_log);

			// join strings seamlessly
            for (int i = 0; strings[i] != NULL; i++)
				log_buffer_push_bytes((CHAR*)strings[i], (lengths ? lengths[i] : wcslen(strings[i])) * sizeof(WCHAR), &write_ptr, session->monitor_log);
		}
		else // this can only happen when the entire buffer is to small to hold this one entry
			Log_Msg0(MSG_MONITOR_OVERFLOW);
    }

    Session_Unlock(irql);
}


//---------------------------------------------------------------------------
// Session_Api_MonitorControl
//---------------------------------------------------------------------------


_FX NTSTATUS Session_Api_MonitorControl(PROCESS *proc, ULONG64 *parms)
{
    API_MONITOR_CONTROL_ARGS *args = (API_MONITOR_CONTROL_ARGS *)parms;
    ULONG *in_flag;
    ULONG *out_flag;
    ULONG *out_used;
    SESSION *session;
    KIRQL irql;
    BOOLEAN EnableMonitor;

    if (proc)
        return STATUS_NOT_IMPLEMENTED;

    //
    // get status
    //

    out_flag = args->get_flag.val;
    if (out_flag) {
        ProbeForWrite(out_flag, sizeof(ULONG), sizeof(ULONG));
        *out_flag = FALSE;
        session = Session_Get(FALSE, -1, &irql);
        if (session) {
            if (session->monitor_log)
                *out_flag = TRUE;
            Session_Unlock(irql);
        }
    }

    //out_used = args->get_used.val;
    //if (out_used) {
    //    ProbeForWrite(out_used, sizeof(ULONG), sizeof(ULONG));
    //    *out_used = 0;
    //    session = Session_Get(FALSE, -1, &irql);
    //    if (session) {
    //        if (session->monitor_log)
    //            *out_used = (ULONG)session->monitor_log->buffer_used;
    //        Session_Unlock(irql);
    //    }
    //}

    //
    // set status
    //

    in_flag = args->set_flag.val;
    if (in_flag) {
        ProbeForRead(in_flag, sizeof(ULONG), sizeof(ULONG));
        if (*in_flag) {

            if (! Session_CheckAdminAccess(L"MonitorAdminOnly"))
                return STATUS_ACCESS_DENIED;

            EnableMonitor = TRUE;

        } else
            EnableMonitor = FALSE;

        session = Session_Get(FALSE, -1, &irql);
        if (session) {

            if (EnableMonitor && (! session->monitor_log)) {

                ULONG BuffSize = Conf_Get_Number(NULL, L"TraceBufferPages", 0, 256) * PAGE_SIZE;

				session->monitor_log = log_buffer_init(BuffSize * sizeof(WCHAR));
                if (!session->monitor_log) {
                    Log_Msg0(MSG_1201);
                    session->monitor_log = log_buffer_init(SESSION_MONITOR_BUF_SIZE * sizeof(WCHAR));
                }

                if (session->monitor_log) {
                    InterlockedIncrement(&Session_MonitorCount);
                } else
                    Log_Msg0(MSG_1201);

            } else if ((! EnableMonitor) && session->monitor_log) {

				log_buffer_free(session->monitor_log);
				session->monitor_log = NULL;
                InterlockedDecrement(&Session_MonitorCount);
            }

            Session_Unlock(irql);
        }
    }

    return STATUS_SUCCESS;
}


//---------------------------------------------------------------------------
// Session_Api_MonitorPut
//---------------------------------------------------------------------------


//_FX NTSTATUS Session_Api_MonitorPut(PROCESS *proc, ULONG64 *parms)
//{
//    API_MONITOR_GET_PUT_ARGS *args = (API_MONITOR_GET_PUT_ARGS *)parms;
//    API_MONITOR_PUT2_ARGS args2 = { args->func_code, args->log_type.val64, args->log_len.val64, args->log_ptr.val64, TRUE, 0 };
//
//    return Session_Api_MonitorPut2(proc, (ULONG64*)&args2);
//}


//---------------------------------------------------------------------------
// Session_Api_MonitorPut
//---------------------------------------------------------------------------


_FX NTSTATUS Session_Api_MonitorPut2(PROCESS *proc, ULONG64 *parms)
{
    API_MONITOR_PUT2_ARGS *args = (API_MONITOR_PUT2_ARGS *)parms;
    ULONG log_type;
    WCHAR *log_data;
    WCHAR *name;
    NTSTATUS status;
    ULONG log_len;

    if (! proc)
        return STATUS_NOT_IMPLEMENTED;

    if (! Session_MonitorCount)
        return STATUS_SUCCESS;

    log_type = args->log_type.val;
    if (!log_type)
        return STATUS_INVALID_PARAMETER;

	log_len = args->log_len.val / sizeof(WCHAR);
    if (!log_len)
        return STATUS_INVALID_PARAMETER;

    log_data = args->log_ptr.val;
    ProbeForRead(log_data, log_len * sizeof(WCHAR), sizeof(WCHAR));

    //
    // if we dont need to check_object_exists we can use a shortcut
    //

    if (!args->check_object_exists.val64){ 
        const WCHAR* strings[2] = { log_data, NULL };
        ULONG lengths[2] = { log_len, 0 };
        Session_MonitorPutEx(log_type | MONITOR_USER, strings, lengths, proc->pid, PsGetCurrentThreadId());
        return STATUS_SUCCESS;
    }

    const ULONG max_buff = 2048;
	if (log_len > max_buff) // truncate as we only have 1028 in buffer
		log_len = max_buff;
    name = Mem_Alloc(proc->pool, (max_buff + 4) * sizeof(WCHAR)); // todo: should we increase this ?
    if (! name)
        return STATUS_INSUFFICIENT_RESOURCES;

    //
    // we do everything else within a try/except block to make sure
    // that we always free the name buffer regardless of errors
    //

    __try {

        wmemcpy(name, log_data, log_len);
        name[log_len] = L'\0';

        status = STATUS_SUCCESS;

        if (args->check_object_exists.val64 && ((log_type & MONITOR_TRACE) == 0)) { // do not check objects if this is a trace entry

            UNICODE_STRING objname;
            void* object = NULL;

            //
            // if type is MONITOR_IPC we try to open the object
            // to get the name assigned to it at time of creation
            //

            if ((log_type & MONITOR_TYPE_MASK) == MONITOR_IPC) {

                ULONG i;

                RtlInitUnicodeString(&objname, name);

                for (i = 0; Session_ObjectTypes[i]; ++i) {

                    // ObReferenceObjectByName needs a non-zero ObjectType
                    // so we have to keep going through all possible object
                    // types as long as we get STATUS_OBJECT_TYPE_MISMATCH

                    status = ObReferenceObjectByName(
                                &objname, OBJ_CASE_INSENSITIVE, NULL, 0,
                                Session_ObjectTypes[i], KernelMode, NULL,
                                &object);

                    if (status != STATUS_OBJECT_TYPE_MISMATCH)
                        break;
                }

                // DbgPrint("IPC  Status = %08X Object = %08X for Open <%S>\n", status, object, name);
            }

            //
            // if type is MONITOR_PIPE we try to open the pipe
            // to get the name assigned to it at time of creation
            //

            if ((log_type & MONITOR_TYPE_MASK) == MONITOR_PIPE) {

                OBJECT_ATTRIBUTES objattrs;
                IO_STATUS_BLOCK IoStatusBlock;
                HANDLE handle;

                InitializeObjectAttributes(&objattrs,
                    &objname, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                    NULL, NULL);

                RtlInitUnicodeString(&objname, name);

                status = IoCreateFileSpecifyDeviceObjectHint(
                    &handle, 0, &objattrs, &IoStatusBlock,
                    NULL, 0, FILE_SHARE_VALID_FLAGS, FILE_OPEN, 0,
                    NULL, 0, CreateFileTypeNone, NULL,
                    IO_IGNORE_SHARE_ACCESS_CHECK, NULL);

                if (NT_SUCCESS(status)) {

                    status = ObReferenceObjectByHandle(
                        handle, 0, NULL, KernelMode, &object, NULL);

                    ZwClose(handle);

                }
                else if (status == STATUS_UNSUCCESSFUL
                    || status == STATUS_PIPE_NOT_AVAILABLE
                    || status == STATUS_NOT_SUPPORTED) {

                    //
                    // might be a strange device like \Device\NDMP4 which can't
                    // be opened, change the status to prevent logging of an
                    // error entry (i.e. question mark, see below)
                    //

                    status = STATUS_OBJECT_NAME_NOT_FOUND;
                }

                //DbgPrint("PIPE Status3 = %08X Object = %08X for Open <%S>\n", status, object, name);
            }

            //
            // if we have an object, get its name from the kernel object
            //

            if (NT_SUCCESS(status) && object) {

                OBJECT_NAME_INFORMATION *Name;
                ULONG NameLength;

                status = Obj_GetNameOrFileName(
                                        proc->pool, object, &Name, &NameLength);

                if (NT_SUCCESS(status)) {

				    log_len = Name->Name.Length / sizeof(WCHAR);
                    if (log_len > max_buff) // truncate as we only have 1028 in buffer
					    log_len = max_buff;
                    wmemcpy(name, Name->Name.Buffer, log_len);
                    name[log_len] = L'\0';

                    if (Name != &Obj_Unnamed)
                        Mem_Free(Name, NameLength);

                    // DbgPrint("Determined Object Name <%S>\n", name);
                }

                ObDereferenceObject(object);
            }

        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    //
    // append the object name into to the monitor log
    //

    if (    status != STATUS_OBJECT_NAME_NOT_FOUND
         && status != STATUS_OBJECT_PATH_NOT_FOUND
         && status != STATUS_OBJECT_PATH_SYNTAX_BAD) {

        if (! NT_SUCCESS(status)) {
            name[0] = L'?';
            name[1] = L'\0';
        }

        const WCHAR* strings[2] = { name, NULL };
        Session_MonitorPutEx(log_type | MONITOR_USER, strings, NULL, proc->pid, PsGetCurrentThreadId());
    }

    Mem_Free(name, (max_buff + 4) * sizeof(WCHAR));

    return STATUS_SUCCESS;
}


//---------------------------------------------------------------------------
// Session_Api_MonitorGet
//---------------------------------------------------------------------------

//_FX NTSTATUS Session_Api_MonitorGet(PROCESS *proc, ULONG64 *parms)
//{
//	API_MONITOR_GET_PUT_ARGS *args = (API_MONITOR_GET_PUT_ARGS *)parms;
//	API_MONITOR_GET_EX_ARGS args2 = { args->func_code, 0, args->log_type.val64, 0, args->log_len.val64, args->log_ptr.val64 };
//
//	return Session_Api_MonitorGetEx(proc, (ULONG64*)&args2);
//}

//---------------------------------------------------------------------------
// Session_Api_MonitorGetEx
//---------------------------------------------------------------------------

_FX NTSTATUS Session_Api_MonitorGetEx(PROCESS* proc, ULONG64* parms)
{
    API_MONITOR_GET_EX_ARGS* args = (API_MONITOR_GET_EX_ARGS*)parms;
    NTSTATUS status;
    ULONG* seq_num;
    ULONG* log_type;
    ULONG* log_pid;
    ULONG* log_tid;
    UNICODE_STRING64* log_data;
    WCHAR* log_buffer;
    SESSION* session;
    KIRQL irql;

    if (proc)
        return STATUS_NOT_IMPLEMENTED;

    seq_num = args->log_seq.val;
    if (seq_num != NULL) {
        ProbeForRead(seq_num, sizeof(ULONG), sizeof(ULONG));
        ProbeForWrite(seq_num, sizeof(ULONG), sizeof(ULONG));
    }

    log_type = args->log_type.val;
    ProbeForWrite(log_type, sizeof(ULONG), sizeof(ULONG));
    *log_type = 0;

    log_pid = args->log_pid.val;
    if (log_pid != NULL) {
        ProbeForWrite(log_pid, sizeof(ULONG), sizeof(ULONG));
        *log_pid = 0;
    }

    log_tid = args->log_tid.val;
    if (log_tid != NULL) {
        ProbeForWrite(log_tid, sizeof(ULONG), sizeof(ULONG));
        *log_tid = 0;
    }

    log_data = args->log_data.val;
    if (!log_data)
        return STATUS_INVALID_PARAMETER;
    ProbeForRead(log_data, sizeof(UNICODE_STRING64), sizeof(ULONG));
    ProbeForWrite(log_data, sizeof(UNICODE_STRING64), sizeof(ULONG));

    log_buffer = (WCHAR*)log_data->Buffer;
    if (!log_buffer)
        return STATUS_INVALID_PARAMETER;
    
    status = STATUS_SUCCESS;

    session = Session_Get(FALSE, -1, &irql);
    if (!session)
        return STATUS_UNSUCCESSFUL;

    __try {

        if (!session->monitor_log) {

            status = STATUS_DEVICE_NOT_READY;
            __leave;
        }

        CHAR* read_ptr = NULL;
        if (seq_num != NULL)
            read_ptr = log_buffer_get_next(*seq_num, session->monitor_log);
        else if (session->monitor_log->buffer_size > 0) // for compatibility with older versions we return the oldest entry
            read_ptr = session->monitor_log->buffer_start_ptr;

        if (!read_ptr) {

            status = STATUS_NO_MORE_ENTRIES;
            __leave;
        }

        LOG_BUFFER_SIZE_T entry_size = log_buffer_get_size(&read_ptr, session->monitor_log);
        LOG_BUFFER_SEQ_T seq_number = log_buffer_get_seq_num(&read_ptr, session->monitor_log);

        //if (seq_num != NULL && seq_number != *seq_num + 1) {
        //
        //	status = STATUS_REQUEST_OUT_OF_SEQUENCE;
        //	*seq_num = seq_number - 1;
        //	__leave;
        //}

        //[Type 4][PID 4][TID 4][Data n*2]

        log_buffer_get_bytes((CHAR*)log_type, 4, &read_ptr, session->monitor_log);

        ULONG pid;
        log_buffer_get_bytes((CHAR*)&pid, 4, &read_ptr, session->monitor_log);
        if (log_pid != NULL)
            *log_pid = pid;

        ULONG tid;
        log_buffer_get_bytes((CHAR*)&tid, 4, &read_ptr, session->monitor_log);
        if (log_tid != NULL)
            *log_tid = tid;

        ULONG data_size = (entry_size - (4 + 4 + 4));
        if ((USHORT)data_size > (log_data->MaximumLength - 1))
        {
            data_size = (log_data->MaximumLength - 1);
            status = STATUS_BUFFER_TOO_SMALL;
        }
        
        log_data->Length = (USHORT)data_size;
        ProbeForWrite(log_buffer, data_size + 1, sizeof(WCHAR));
        memcpy(log_buffer, read_ptr, data_size);

        log_buffer[data_size / sizeof(wchar_t)] = L'\0';
        

        if (seq_num != NULL)
            *seq_num = seq_number;
        else // for compatibility with older versions we fall back to clearing the returned entry
            log_buffer_pop_entry(session->monitor_log);

    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    Session_Unlock(irql);

    return status;
}
