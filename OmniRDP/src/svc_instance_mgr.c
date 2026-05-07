/**
 * @file svc_instance_mgr.c
 * @brief Instance manager implementation for OmniRDP service
 *
 * Manages child OmniRDP.exe processes:
 * - Spawning with --instance --secrets-handle --config flags
 * - Password transfer via anonymous pipe (DPAPI-decrypted)
 * - Heartbeat monitoring via named pipes
 * - Graceful shutdown (CTRL_BREAK_EVENT) with fallback to TerminateProcess
 * - Automatic reconnection with exponential backoff
 * - Hot-reload of config (diff + apply)
 *
 * Windows-only; no FreeRDP dependency.
 */

#include "svc_instance_mgr.h"
#include "svc_dpapi.h"
#include "svc_log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <aclapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ──────────────────────────────────────────── */

/**
 * @brief Build the heartbeat pipe name for an instance
 * \\.\pipe\OmniRDP_Instance_<name>
 */
static void
build_heartbeat_pipe_name(const char* instanceName, char* buf, size_t bufSize)
{
	_snprintf(buf, bufSize, "\\\\.\\pipe\\OmniRDP_Instance_%s", instanceName);
	buf[bufSize - 1] = '\0';
}

/**
 * @brief Map InstanceState to PipeInstanceState for the IPC protocol
 */
static PipeInstanceState
map_state_to_pipe(InstanceState s)
{
	switch (s)
	{
		case INST_STOPPED:
			return INSTANCE_STOPPED;
		case INST_STARTING:
			return INSTANCE_STARTING;
		case INST_RUNNING:
			return INSTANCE_RUNNING;
		case INST_RECONNECTING:
			return INSTANCE_RECONNECTING;
		default:
			return INSTANCE_STOPPED;
	}
}

/**
 * @brief Calculate the next reconnect delay with exponential backoff
 *
 * delay = min(initial_delay * (multiplier ^ attempt), max_delay)
 */
static ULONGLONG
calc_reconnect_delay(const InstanceConfig* cfg, unsigned int attempt)
{
	double delay = (double)cfg->reconnect_initial_delay_ms;
	for (unsigned int i = 0; i < attempt; i++)
	{
		delay *= cfg->reconnect_backoff_multiplier;
		if (delay >= (double)cfg->reconnect_max_delay_ms)
		{
			delay = (double)cfg->reconnect_max_delay_ms;
			break;
		}
	}
	return (ULONGLONG)delay;
}

/* ── inst_mgr_init ─────────────────────────────────────────────── */

int
inst_mgr_init(InstanceManager* mgr, SvcConfig* config, const char* configPath, const char* exePath)
{
	if (!mgr || !config || !configPath || !exePath)
		return -1;

	memset(mgr, 0, sizeof(*mgr));

	mgr->instances = (ManagedInstance*)calloc(config->instance_count, sizeof(ManagedInstance));
	if (!mgr->instances && config->instance_count > 0)
		return -1;

	mgr->instanceCount = config->instance_count;
	mgr->config = config;
	mgr->configPath = configPath;
	mgr->exePath = exePath;
	mgr->shuttingDown = FALSE;

	InitializeCriticalSection(&mgr->lock);

	for (unsigned int i = 0; i < mgr->instanceCount; i++)
	{
		ManagedInstance* inst = &mgr->instances[i];
		memset(inst, 0, sizeof(*inst));

		/* Copy name and config */
		strncpy(inst->name, config->instances[i].name, sizeof(inst->name) - 1);
		inst->name[sizeof(inst->name) - 1] = '\0';
		memcpy(&inst->config, &config->instances[i], sizeof(InstanceConfig));

		/* Default state */
		inst->state = INST_STOPPED;

		/* Handles start as NULL / 0 */
		inst->hProcess = NULL;
		inst->hJob = NULL;
		inst->pid = 0;
		inst->hHeartbeatPipe = NULL;
		inst->lastHeartbeatMs = 0;
		inst->reconnectAttempts = 0;
		inst->nextReconnectMs = 0;
		inst->viewerCount = 0;
		inst->stopRequested = FALSE;
	}

	return 0;
}

/* ── inst_mgr_find ─────────────────────────────────────────────── */

ManagedInstance*
inst_mgr_find(InstanceManager* mgr, const char* name)
{
	if (!mgr || !name)
		return NULL;

	for (unsigned int i = 0; i < mgr->instanceCount; i++)
	{
		if (strcmp(mgr->instances[i].name, name) == 0)
			return &mgr->instances[i];
	}
	return NULL;
}

/* ── inst_mgr_start (single instance) ──────────────────────────── */

int
inst_mgr_start(InstanceManager* mgr, const char* instanceName)
{
	if (!mgr || !instanceName)
		return -1;

	if (mgr->shuttingDown)
	{
		LOG_W("svc_inst_mgr", "Cannot start instance '%s': service is shutting down", instanceName);
		return -1;
	}

	EnterCriticalSection(&mgr->lock);

	ManagedInstance* inst = inst_mgr_find(mgr, instanceName);
	if (!inst)
	{
		LOG_E("svc_inst_mgr", "Start: instance '%s' not found", instanceName);
		LeaveCriticalSection(&mgr->lock);
		return -1;
	}

	if (inst->state != INST_STOPPED)
	{
		LOG_W("svc_inst_mgr", "Start: instance '%s' already running (state=%d)", instanceName,
		      (int)inst->state);
		LeaveCriticalSection(&mgr->lock);
		return -1;
	}

	/* ── Mark as starting ───────────────────────────────────── */
	inst->state = INST_STARTING;

	/* ── Decrypt / prepare password ──────────────────────────── */
	char decrypted_password[1024];
	SecureZeroMemory(decrypted_password, sizeof(decrypted_password));

	if (svc_dpapi_is_encrypted(inst->config.backend_password))
	{
		/* Already encrypted in config – decrypt */
		if (svc_dpapi_decrypt(inst->config.backend_password, decrypted_password,
		                      sizeof(decrypted_password)) != 0)
		{
			LOG_E("svc_inst_mgr", "Start: failed to decrypt password for '%s'", instanceName);
			inst->state = INST_STOPPED;
			LeaveCriticalSection(&mgr->lock);
			return -1;
		}
	}
	else
	{
		/* Plaintext – encrypt in config file first, then decrypt via round-trip */
		if (svc_dpapi_encrypt_in_file(mgr->configPath, instanceName, "backend.password") != 0)
		{
			LOG_W("svc_inst_mgr",
			      "Start: failed to encrypt password in config "
			      "file for '%s' (continuing)",
			      instanceName);
		}

		/* Round-trip: encrypt plaintext → dpapi, then decrypt back */
		char encrypted_buf[4096];
		SecureZeroMemory(encrypted_buf, sizeof(encrypted_buf));

		if (svc_dpapi_encrypt(inst->config.backend_password, encrypted_buf,
		                      sizeof(encrypted_buf)) != 0)
		{
			LOG_E("svc_inst_mgr", "Start: failed to encrypt password for '%s'", instanceName);
			inst->state = INST_STOPPED;
			LeaveCriticalSection(&mgr->lock);
			return -1;
		}

		if (svc_dpapi_decrypt(encrypted_buf, decrypted_password, sizeof(decrypted_password)) != 0)
		{
			LOG_E("svc_inst_mgr", "Start: failed to re-decrypt password for '%s'", instanceName);
			SecureZeroMemory(encrypted_buf, sizeof(encrypted_buf));
			inst->state = INST_STOPPED;
			LeaveCriticalSection(&mgr->lock);
			return -1;
		}

		SecureZeroMemory(encrypted_buf, sizeof(encrypted_buf));
	}

	/* ── Create anonymous pipe for secret transfer ────────────── */
	HANDLE hPipeRead = NULL;
	HANDLE hPipeWrite = NULL;

	/* Create a restricted security descriptor for the password pipe.
	 * Only the current process token's SID gets access. */
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	/* Build a DACL that grants access only to the current user */
	HANDLE hToken = NULL;
	DWORD tokenInfoLen = 0;
	PSECURITY_DESCRIPTOR pSD = NULL;
	PACL pACL = NULL;

	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
	{
		GetTokenInformation(hToken, TokenUser, NULL, 0, &tokenInfoLen);
		PTOKEN_USER pTokenUser = (PTOKEN_USER)HeapAlloc(GetProcessHeap(), 0, tokenInfoLen);
		if (pTokenUser &&
		    GetTokenInformation(hToken, TokenUser, pTokenUser, tokenInfoLen, &tokenInfoLen))
		{
			EXPLICIT_ACCESSA ea = { 0 };
			ea.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
			ea.grfAccessMode = SET_ACCESS;
			ea.grfInheritance = NO_INHERITANCE;
			ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
			ea.Trustee.ptstrName = (LPSTR)pTokenUser->User.Sid;

			if (SetEntriesInAclA(1, &ea, NULL, &pACL) == ERROR_SUCCESS)
			{
				pSD = (PSECURITY_DESCRIPTOR)HeapAlloc(GetProcessHeap(), 0,
				                                      SECURITY_DESCRIPTOR_MIN_LENGTH);
				if (pSD)
				{
					InitializeSecurityDescriptor(pSD, SECURITY_DESCRIPTOR_REVISION);
					SetSecurityDescriptorDacl(pSD, TRUE, pACL, FALSE);
					sa.lpSecurityDescriptor = pSD;
				}
			}
		}
		if (pTokenUser)
			HeapFree(GetProcessHeap(), 0, pTokenUser);
		CloseHandle(hToken);
	}

	/* Fallback: if anything failed, use NULL descriptor (inherits process
	 * default) */
	if (!sa.lpSecurityDescriptor)
	{
		sa.lpSecurityDescriptor = NULL;
	}

	if (!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 4096))
	{
		LOG_E("svc_inst_mgr", "Start: CreatePipe failed for '%s' (err=%lu)", instanceName,
		      GetLastError());
		if (pSD)
			HeapFree(GetProcessHeap(), 0, pSD);
		if (pACL)
			LocalFree(pACL);
		SecureZeroMemory(decrypted_password, sizeof(decrypted_password));
		inst->state = INST_STOPPED;
		LeaveCriticalSection(&mgr->lock);
		return -1;
	}

	/* Make the read end inheritable (CreatePipe already set this via SA) */
	if (!SetHandleInformation(hPipeRead, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
	{
		LOG_W("svc_inst_mgr",
		      "Start: SetHandleInformation read end failed "
		      "for '%s' (err=%lu)",
		      instanceName, GetLastError());
	}

	/* ── Build command line ────────────────────────────────────── */
	/*
	 * Format:
	 *   "<exePath>" --instance "<name>" --secrets-handle <handle> --config
	 * "<configPath>"
	 */
	char cmdline[32768];
	int cmdlen = _snprintf(cmdline, sizeof(cmdline),
	                       "\"%s\" --instance \"%s\" --secrets-handle %Iu --config \"%s\"",
	                       mgr->exePath, instanceName, (SIZE_T)hPipeRead, mgr->configPath);
	if (cmdlen < 0 || (size_t)cmdlen >= sizeof(cmdline))
	{
		LOG_E("svc_inst_mgr", "Start: command line too long for '%s'", instanceName);
		CloseHandle(hPipeRead);
		CloseHandle(hPipeWrite);
		if (pSD)
			HeapFree(GetProcessHeap(), 0, pSD);
		if (pACL)
			LocalFree(pACL);
		SecureZeroMemory(decrypted_password, sizeof(decrypted_password));
		inst->state = INST_STOPPED;
		LeaveCriticalSection(&mgr->lock);
		return -1;
	}

	/* ── Create heartbeat named pipe for this instance ─────────── */
	{
		char heartbeatPipeName[256];
		build_heartbeat_pipe_name(instanceName, heartbeatPipeName, sizeof(heartbeatPipeName));
		inst->hHeartbeatPipe =
		    CreateNamedPipeA(heartbeatPipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
		                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 65536, 0, NULL);
		if (inst->hHeartbeatPipe == INVALID_HANDLE_VALUE)
		{
			LOG_W("inst_mgr", "Failed to create heartbeat pipe for '%s' (error %lu)", instanceName,
			      GetLastError());
			inst->hHeartbeatPipe = NULL;
		}
	}

	/* ── Create Job Object ──────────────────────────────────────── */
	HANDLE hJob = CreateJobObject(NULL, NULL);
	if (!hJob)
	{
		LOG_E("svc_inst_mgr", "Start: CreateJobObject failed for '%s' (err=%lu)", instanceName,
		      GetLastError());
		CloseHandle(hPipeRead);
		CloseHandle(hPipeWrite);
		if (pSD)
			HeapFree(GetProcessHeap(), 0, pSD);
		if (pACL)
			LocalFree(pACL);
		SecureZeroMemory(decrypted_password, sizeof(decrypted_password));
		inst->state = INST_STOPPED;
		LeaveCriticalSection(&mgr->lock);
		return -1;
	}

	JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo;
	memset(&jobInfo, 0, sizeof(jobInfo));
	jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

	if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jobInfo,
	                             sizeof(jobInfo)))
	{
		LOG_W("svc_inst_mgr",
		      "Start: SetInformationJobObject failed for '%s' "
		      "(err=%lu)",
		      instanceName, GetLastError());
		/* Continue anyway – job without kill-on-close is still useful */
	}

	/* ── CreateProcess ──────────────────────────────────────────── */
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	memset(&pi, 0, sizeof(pi));

	BOOL created =
	    CreateProcessA(mgr->exePath,         /* lpApplicationName */
	                   cmdline,              /* lpCommandLine */
	                   NULL,                 /* lpProcessAttributes */
	                   NULL,                 /* lpThreadAttributes */
	                   TRUE,                 /* bInheritHandles = TRUE (for pipe) */
	                   CREATE_NEW_CONSOLE |  /* Creation flags: separate console for CTRL_BREAK */
	                       CREATE_SUSPENDED, /* Start suspended so we can assign to job first */
	                   NULL,                 /* lpEnvironment */
	                   NULL,                 /* lpCurrentDirectory */
	                   &si,                  /* lpStartupInfo */
	                   &pi);                 /* lpProcessInformation */

	if (!created)
	{
		LOG_E("svc_inst_mgr", "Start: CreateProcess failed for '%s' (err=%lu)", instanceName,
		      GetLastError());
		CloseHandle(hJob);
		CloseHandle(hPipeRead);
		CloseHandle(hPipeWrite);
		if (pSD)
			HeapFree(GetProcessHeap(), 0, pSD);
		if (pACL)
			LocalFree(pACL);
		SecureZeroMemory(decrypted_password, sizeof(decrypted_password));
		inst->state = INST_STOPPED;
		LeaveCriticalSection(&mgr->lock);
		return -1;
	}

	/* Assign child to job object */
	if (!AssignProcessToJobObject(hJob, pi.hProcess))
	{
		LOG_W("svc_inst_mgr",
		      "Start: AssignProcessToJobObject failed for '%s' "
		      "(err=%lu)",
		      instanceName, GetLastError());
		/* Continue anyway – we still track the process */
	}

	/* Resume the suspended process */
	ResumeThread(pi.hThread);

	/* ── Write password to pipe ─────────────────────────────────── */
	DWORD written = 0;
	size_t pwd_len = strlen(decrypted_password);
	if (!WriteFile(hPipeWrite, decrypted_password, (DWORD)pwd_len, &written, NULL))
	{
		LOG_E("svc_inst_mgr",
		      "Start: WriteFile to secret pipe failed for '%s' "
		      "(err=%lu)",
		      instanceName, GetLastError());
		/* Child process will fail to read – terminate */
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		CloseHandle(hJob);
		CloseHandle(hPipeRead);
		CloseHandle(hPipeWrite);
		if (pSD)
			HeapFree(GetProcessHeap(), 0, pSD);
		if (pACL)
			LocalFree(pACL);
		SecureZeroMemory(decrypted_password, sizeof(decrypted_password));
		inst->state = INST_STOPPED;
		LeaveCriticalSection(&mgr->lock);
		return -1;
	}

	/* Close the write end – child has the read end via inherited handle */
	CloseHandle(hPipeWrite);
	hPipeWrite = NULL;

	/* Clean up security descriptor */
	if (pSD)
		HeapFree(GetProcessHeap(), 0, pSD);
	if (pACL)
		LocalFree(pACL);

	/* ── Store handles and state ────────────────────────────────── */
	inst->hProcess = pi.hProcess;
	inst->hJob = hJob;
	inst->pid = pi.dwProcessId;
	inst->lastHeartbeatMs = GetTickCount64();
	inst->reconnectAttempts = 0;
	inst->nextReconnectMs = 0;
	inst->viewerCount = 0;
	inst->stopRequested = FALSE;
	inst->state = INST_RUNNING;

	/* Close the thread handle – we don't need it */
	CloseHandle(pi.hThread);

	/* Close our copy of the read end – child inherited it */
	CloseHandle(hPipeRead);

	/* ── Clean up password buffer ───────────────────────────────── */
	SecureZeroMemory(decrypted_password, sizeof(decrypted_password));

	LOG_I("svc_inst_mgr", "Started instance '%s' (PID=%lu, job=%p)", instanceName, inst->pid,
	      (void*)inst->hJob);

	LeaveCriticalSection(&mgr->lock);
	return 0;
}

/* ── inst_mgr_stop (single instance) ───────────────────────────── */

int
inst_mgr_stop(InstanceManager* mgr, const char* instanceName)
{
	if (!mgr || !instanceName)
		return -1;

	EnterCriticalSection(&mgr->lock);

	ManagedInstance* inst = inst_mgr_find(mgr, instanceName);
	if (!inst)
	{
		LOG_E("svc_inst_mgr", "Stop: instance '%s' not found", instanceName);
		LeaveCriticalSection(&mgr->lock);
		return -1;
	}

	if (inst->state == INST_STOPPED)
	{
		LOG_D("svc_inst_mgr", "Stop: instance '%s' already stopped", instanceName);
		LeaveCriticalSection(&mgr->lock);
		return 0;
	}

	inst->stopRequested = TRUE;

	HANDLE hProcess = inst->hProcess;
	DWORD pid = inst->pid;
	char name_copy[128];
	strncpy(name_copy, inst->name, sizeof(name_copy) - 1);
	name_copy[sizeof(name_copy) - 1] = '\0';

	/* Set state before releasing lock so poll() doesn't interfere */
	inst->state = INST_STOPPED;
	inst->hProcess = NULL;
	inst->pid = 0;

	LeaveCriticalSection(&mgr->lock);

	LOG_I("svc_inst_mgr", "Stopping instance '%s' (PID=%lu)...", name_copy, pid);

	/* ── Send CTRL_BREAK_EVENT ──────────────────────────────────── */
	if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid))
	{
		LOG_W("svc_inst_mgr",
		      "Stop: GenerateConsoleCtrlEvent failed for '%s' "
		      "(err=%lu)",
		      name_copy, GetLastError());
	}

	/* ── Wait for graceful shutdown ─────────────────────────────── */
	DWORD timeout_ms = mgr->config->service.graceful_shutdown_sec * 1000;
	DWORD waitResult = WaitForSingleObject(hProcess, timeout_ms);

	if (waitResult == WAIT_TIMEOUT)
	{
		LOG_W("svc_inst_mgr",
		      "Stop: instance '%s' did not exit within %u sec, "
		      "terminating",
		      name_copy, mgr->config->service.graceful_shutdown_sec);
		TerminateProcess(hProcess, 1);
		WaitForSingleObject(hProcess, 5000); /* Brief wait for cleanup */
	}
	else if (waitResult == WAIT_OBJECT_0)
	{
		LOG_I("svc_inst_mgr", "Stop: instance '%s' exited gracefully", name_copy);
	}
	else
	{
		LOG_E("svc_inst_mgr",
		      "Stop: unexpected WaitForSingleObject result for "
		      "'%s' (%lu)",
		      name_copy, GetLastError());
	}

	/* ── Close handles ──────────────────────────────────────────── */
	CloseHandle(hProcess);

	EnterCriticalSection(&mgr->lock);

	/* Also close the job handle if still attached */
	if (inst->hJob)
	{
		CloseHandle(inst->hJob);
		inst->hJob = NULL;
	}
	if (inst->hHeartbeatPipe)
	{
		CloseHandle(inst->hHeartbeatPipe);
		inst->hHeartbeatPipe = NULL;
	}

	inst->lastHeartbeatMs = 0;
	inst->reconnectAttempts = 0;
	inst->nextReconnectMs = 0;
	inst->stopRequested = FALSE;

	LeaveCriticalSection(&mgr->lock);

	LOG_I("svc_inst_mgr", "Instance '%s' stopped", name_copy);
	return 0;
}

/* ── inst_mgr_restart ──────────────────────────────────────────── */

int
inst_mgr_restart(InstanceManager* mgr, const char* instanceName)
{
	if (!mgr || !instanceName)
		return -1;

	LOG_I("svc_inst_mgr", "Restarting instance '%s'...", instanceName);

	if (inst_mgr_stop(mgr, instanceName) != 0)
	{
		LOG_W("svc_inst_mgr",
		      "Restart: stop failed for '%s', attempting start "
		      "anyway",
		      instanceName);
	}

	/* Brief delay to allow resources to be released */
	Sleep(200);

	return inst_mgr_start(mgr, instanceName);
}

/* ── inst_mgr_start_all ────────────────────────────────────────── */

void
inst_mgr_start_all(InstanceManager* mgr)
{
	if (!mgr)
		return;

	LOG_I("svc_inst_mgr", "Starting all enabled instances (%u total)...", mgr->instanceCount);

	for (unsigned int i = 0; i < mgr->instanceCount; i++)
	{
		if (!mgr->instances[i].config.enabled)
		{
			LOG_D("svc_inst_mgr", "Skipping disabled instance '%s'", mgr->instances[i].name);
			continue;
		}

		if (inst_mgr_start(mgr, mgr->instances[i].name) != 0)
		{
			LOG_E("svc_inst_mgr", "Failed to start instance '%s'", mgr->instances[i].name);
		}

		/* Delay between spawns if configured and not the last instance */
		if (mgr->config->service.instance_startup_delay_ms > 0 && i + 1 < mgr->instanceCount)
		{
			Sleep(mgr->config->service.instance_startup_delay_ms);
		}
	}
}

/* ── inst_mgr_stop_all ─────────────────────────────────────────── */

void
inst_mgr_stop_all(InstanceManager* mgr)
{
	if (!mgr)
		return;

	EnterCriticalSection(&mgr->lock);
	mgr->shuttingDown = TRUE;
	LeaveCriticalSection(&mgr->lock);

	LOG_I("svc_inst_mgr", "Stopping all instances...");

	for (unsigned int i = 0; i < mgr->instanceCount; i++)
	{
		if (mgr->instances[i].state != INST_STOPPED)
		{
			inst_mgr_stop(mgr, mgr->instances[i].name);
		}
	}
}

/* ── inst_mgr_poll ─────────────────────────────────────────────── */

void
inst_mgr_poll(InstanceManager* mgr)
{
	if (!mgr)
		return;

	ULONGLONG nowMs = GetTickCount64();

	EnterCriticalSection(&mgr->lock);

	if (mgr->shuttingDown)
	{
		LeaveCriticalSection(&mgr->lock);
		return;
	}

	for (unsigned int i = 0; i < mgr->instanceCount; i++)
	{
		ManagedInstance* inst = &mgr->instances[i];

		/* ── INST_RUNNING: check if process exited / heartbeat timeout ── */
		if (inst->state == INST_RUNNING && inst->hProcess != NULL)
		{
			DWORD exitCode = STILL_ACTIVE;
			BOOL alive = TRUE;

			if (!GetExitCodeProcess(inst->hProcess, &exitCode))
			{
				LOG_W("svc_inst_mgr",
				      "Poll: GetExitCodeProcess failed for '%s' "
				      "(err=%lu)",
				      inst->name, GetLastError());
			}
			else if (exitCode != STILL_ACTIVE)
			{
				alive = FALSE;
				LOG_W("svc_inst_mgr", "Poll: instance '%s' exited (code=%lu)", inst->name,
				      exitCode);
			}

			/* Check heartbeat pipe for activity */
			if (inst->hHeartbeatPipe != NULL)
			{
				DWORD bytesAvail = 0;
				if (PeekNamedPipe(inst->hHeartbeatPipe, NULL, 0, NULL, &bytesAvail, NULL))
				{
					if (bytesAvail > 0)
					{
						/* Child sent a heartbeat — read and discard */
						char buf[256];
						DWORD bytesRead;
						ReadFile(inst->hHeartbeatPipe, buf, sizeof(buf), &bytesRead, NULL);
						inst->lastHeartbeatMs = nowMs;
					}
				}
			}

#if 1
			/* Check heartbeat timeout */
			ULONGLONG timeoutMs = (ULONGLONG)mgr->config->service.heartbeat_timeout_sec * 1000ULL;
			if (inst->lastHeartbeatMs > 0 && (nowMs - inst->lastHeartbeatMs) > timeoutMs)
			{
				LOG_W("svc_inst_mgr",
				      "Poll: heartbeat timeout for '%s' "
				      "(last=%llu, now=%llu, timeout=%llu)",
				      inst->name, inst->lastHeartbeatMs, nowMs, timeoutMs);

				/* Force-kill if process still appears alive */
				if (alive && inst->hProcess != NULL)
				{
					TerminateProcess(inst->hProcess, 1);
					LOG_W("svc_inst_mgr",
					      "Poll: force-killed '%s' due to "
					      "heartbeat timeout",
					      inst->name);
				}
				alive = FALSE;
			}
#endif

			if (!alive)
			{
				/* Clean up handles */
				if (inst->hProcess)
				{
					CloseHandle(inst->hProcess);
					inst->hProcess = NULL;
				}
				if (inst->hJob)
				{
					CloseHandle(inst->hJob);
					inst->hJob = NULL;
				}
				if (inst->hHeartbeatPipe)
				{
					CloseHandle(inst->hHeartbeatPipe);
					inst->hHeartbeatPipe = NULL;
				}

				/* Initiate reconnection if enabled */
				if (inst->config.reconnect_enabled && !inst->stopRequested)
				{
					ULONGLONG delay = calc_reconnect_delay(&inst->config, inst->reconnectAttempts);
					inst->nextReconnectMs = nowMs + delay;
					inst->state = INST_RECONNECTING;
					LOG_I("svc_inst_mgr",
					      "Poll: instance '%s' will reconnect in "
					      "%llu ms (attempt %u)",
					      inst->name, delay, inst->reconnectAttempts + 1);
				}
				else
				{
					inst->state = INST_STOPPED;
					inst->reconnectAttempts = 0;
					LOG_I("svc_inst_mgr",
					      "Poll: instance '%s' stopped "
					      "(reconnect disabled or stop requested)",
					      inst->name);
				}
			}
		}

		/* ── INST_RECONNECTING: attempt reconnection ───────────────── */
		if (inst->state == INST_RECONNECTING)
		{
			if (nowMs >= inst->nextReconnectMs)
			{
				/* Check max attempts */
				if (inst->reconnectAttempts >= inst->config.reconnect_max_attempts)
				{
					LOG_W("svc_inst_mgr",
					      "Poll: instance '%s' reached max "
					      "reconnect attempts (%u), giving up",
					      inst->name, inst->reconnectAttempts);
					inst->state = INST_STOPPED;
					inst->reconnectAttempts = 0;
					continue;
				}

				LeaveCriticalSection(&mgr->lock);
				int rc = inst_mgr_start(mgr, inst->name);
				EnterCriticalSection(&mgr->lock);

				/* Re-fetch pointer in case the array moved (it shouldn't for poll) */
				inst = &mgr->instances[i];

				if (rc == 0)
				{
					LOG_I("svc_inst_mgr",
					      "Poll: instance '%s' reconnected "
					      "successfully after %u attempts",
					      inst->name, inst->reconnectAttempts);
					inst->reconnectAttempts = 0;
					inst->nextReconnectMs = 0;
				}
				else
				{
					inst->reconnectAttempts++;
					ULONGLONG delay = calc_reconnect_delay(&inst->config, inst->reconnectAttempts);
					inst->nextReconnectMs = GetTickCount64() + delay;
					LOG_W("svc_inst_mgr",
					      "Poll: reconnect attempt %u failed for "
					      "'%s', next in %llu ms",
					      inst->reconnectAttempts, inst->name, delay);
				}
			}
		}
	}

	LeaveCriticalSection(&mgr->lock);
}

/* ── inst_mgr_get_info ─────────────────────────────────────────── */

int
inst_mgr_get_info(InstanceManager* mgr, unsigned int index, PipeInstanceInfo* info)
{
	if (!mgr || !info)
		return -1;

	EnterCriticalSection(&mgr->lock);

	if (index >= mgr->instanceCount)
	{
		LeaveCriticalSection(&mgr->lock);
		return -1;
	}

	ManagedInstance* inst = &mgr->instances[index];

	strncpy(info->name, inst->name, sizeof(info->name) - 1);
	info->name[sizeof(info->name) - 1] = '\0';

	info->state = map_state_to_pipe(inst->state);
	info->viewer_count = inst->viewerCount;

	strncpy(info->backend_hostname, inst->config.backend_hostname,
	        sizeof(info->backend_hostname) - 1);
	info->backend_hostname[sizeof(info->backend_hostname) - 1] = '\0';

	info->backend_port = inst->config.backend_port;

	LeaveCriticalSection(&mgr->lock);
	return 0;
}

/* ── inst_mgr_reload_config ────────────────────────────────────── */

int
inst_mgr_reload_config(InstanceManager* mgr, const char* configPath)
{
	if (!mgr || !configPath)
		return -1;

	LOG_I("svc_inst_mgr", "Reloading config from '%s'...", configPath);

	/* ── 1. Load new config ──────────────────────────────────────── */
	SvcConfig* newCfg = svc_config_load(configPath);
	if (!newCfg)
	{
		LOG_E("svc_inst_mgr", "Reload: failed to load config from '%s'", configPath);
		return -1;
	}

	/* ── 2. Validate ────────────────────────────────────────────── */
	/* Check port uniqueness and required fields */
	for (unsigned int i = 0; i < newCfg->instance_count; i++)
	{
		InstanceConfig* inst = &newCfg->instances[i];
		if (inst->backend_hostname[0] == '\0')
		{
			LOG_E("svc_inst_mgr", "Reload: instance '%s' missing backend.hostname", inst->name);
			svc_config_free(newCfg);
			return -1;
		}
		if (inst->backend_username[0] == '\0')
		{
			LOG_E("svc_inst_mgr", "Reload: instance '%s' missing backend.username", inst->name);
			svc_config_free(newCfg);
			return -1;
		}
		if (inst->backend_port == 0)
		{
			LOG_E("svc_inst_mgr", "Reload: instance '%s' missing backend.port", inst->name);
			svc_config_free(newCfg);
			return -1;
		}
		if (inst->viewer_port == 0)
		{
			LOG_E("svc_inst_mgr", "Reload: instance '%s' missing viewer.port", inst->name);
			svc_config_free(newCfg);
			return -1;
		}

		/* Check port uniqueness among instances */
		for (unsigned int j = i + 1; j < newCfg->instance_count; j++)
		{
			if (newCfg->instances[j].viewer_port == inst->viewer_port)
			{
				LOG_E("svc_inst_mgr",
				      "Reload: duplicate viewer.port %u between "
				      "'%s' and '%s'",
				      inst->viewer_port, inst->name, newCfg->instances[j].name);
				svc_config_free(newCfg);
				return -1;
			}
			if (newCfg->instances[j].backend_port == inst->backend_port &&
			    strcmp(newCfg->instances[j].backend_hostname, inst->backend_hostname) == 0)
			{
				LOG_W("svc_inst_mgr",
				      "Reload: duplicate backend %s:%u between "
				      "'%s' and '%s'",
				      inst->backend_hostname, inst->backend_port, inst->name,
				      newCfg->instances[j].name);
				/* Warning only – not a hard error */
			}
		}
	}

	/* ── 3. Stop removed, disabled, and breaking-change instances ── */
	/* (uses old mgr->instances before the swap so inst_mgr_find works) */

	EnterCriticalSection(&mgr->lock);

	SvcConfig* oldCfg = mgr->config;
	ManagedInstance* oldInstances = mgr->instances;
	unsigned int oldCount = mgr->instanceCount;

	LeaveCriticalSection(&mgr->lock);

	/* 3a. Stop removed instances (in old but not in new config) */
	for (unsigned int i = 0; i < oldCount; i++)
	{
		ManagedInstance* oldInst = &oldInstances[i];
		int found = 0;

		for (unsigned int j = 0; j < newCfg->instance_count; j++)
		{
			if (strcmp(newCfg->instances[j].name, oldInst->name) == 0)
			{
				found = 1;
				break;
			}
		}

		if (!found && oldInst->state != INST_STOPPED)
		{
			LOG_I("svc_inst_mgr",
			      "Reload: instance '%s' removed from config, "
			      "stopping",
			      oldInst->name);
			inst_mgr_stop(mgr, oldInst->name);
		}
	}

	/* 3b. Stop disabled instances (enabled→disabled) */
	for (unsigned int i = 0; i < oldCount; i++)
	{
		ManagedInstance* oldInst = &oldInstances[i];

		for (unsigned int j = 0; j < newCfg->instance_count; j++)
		{
			InstanceConfig* nc = &newCfg->instances[j];
			if (strcmp(nc->name, oldInst->name) != 0)
				continue;

			if (oldInst->config.enabled && !nc->enabled && oldInst->state != INST_STOPPED)
			{
				LOG_I("svc_inst_mgr", "Reload: instance '%s' disabled, stopping", oldInst->name);
				inst_mgr_stop(mgr, oldInst->name);
			}
			break;
		}
	}

	/* 3c. Stop instances with breaking changes (will restart after swap) */
	for (unsigned int i = 0; i < oldCount; i++)
	{
		ManagedInstance* oldInst = &oldInstances[i];

		for (unsigned int j = 0; j < newCfg->instance_count; j++)
		{
			InstanceConfig* nc = &newCfg->instances[j];
			if (strcmp(nc->name, oldInst->name) != 0)
				continue;

			if (!nc->enabled || oldInst->state == INST_STOPPED)
				break;

			int is_breaking = 0;

			/* Backend changes */
			if (strcmp(oldInst->config.backend_hostname, nc->backend_hostname) != 0 ||
			    oldInst->config.backend_port != nc->backend_port ||
			    strcmp(oldInst->config.backend_username, nc->backend_username) != 0 ||
			    strcmp(oldInst->config.backend_domain, nc->backend_domain) != 0)
			{
				is_breaking = 1;
			}

			/* Viewer bind/port */
			if (strcmp(oldInst->config.viewer_bind_address, nc->viewer_bind_address) != 0 ||
			    oldInst->config.viewer_port != nc->viewer_port)
			{
				is_breaking = 1;
			}

			/* Display changes */
			if (oldInst->config.display_monitor_count != nc->display_monitor_count ||
			    oldInst->config.display_monitor_width != nc->display_monitor_width ||
			    oldInst->config.display_monitor_height != nc->display_monitor_height ||
			    oldInst->config.display_color_depth != nc->display_color_depth)
			{
				is_breaking = 1;
			}

			/* Codec changes */
			if (oldInst->config.codec_nscodec != nc->codec_nscodec ||
			    oldInst->config.codec_remote_fx != nc->codec_remote_fx ||
			    oldInst->config.codec_graphics_pipeline != nc->codec_graphics_pipeline ||
			    oldInst->config.codec_h264 != nc->codec_h264 ||
			    oldInst->config.codec_avc444 != nc->codec_avc444 ||
			    oldInst->config.codec_avc444v2 != nc->codec_avc444v2 ||
			    oldInst->config.codec_frame_acknowledge != nc->codec_frame_acknowledge)
			{
				is_breaking = 1;
			}

			/* Security changes */
			if (oldInst->config.security_tls_enabled != nc->security_tls_enabled ||
			    oldInst->config.security_nla_enabled != nc->security_nla_enabled ||
			    strcmp(oldInst->config.security_tls_min_version, nc->security_tls_min_version) !=
			        0 ||
			    oldInst->config.security_server_authentication !=
			        nc->security_server_authentication ||
			    oldInst->config.security_ignore_certificate != nc->security_ignore_certificate)
			{
				is_breaking = 1;
			}

			/* Password change */
			if (strcmp(oldInst->config.backend_password, nc->backend_password) != 0)
			{
				is_breaking = 1;
			}

			if (is_breaking)
			{
				LOG_I("svc_inst_mgr",
				      "Reload: breaking change for '%s', "
				      "stopping for restart",
				      nc->name);
				inst_mgr_stop(mgr, nc->name);
			}
			break;
		}
	}

	/* ── 4. Build new instances array and swap ───────────────────── */

	EnterCriticalSection(&mgr->lock);

	ManagedInstance* newInstances =
	    (ManagedInstance*)calloc(newCfg->instance_count, sizeof(ManagedInstance));
	if (!newInstances)
	{
		LeaveCriticalSection(&mgr->lock);
		svc_config_free(newCfg);
		LOG_E("svc_inst_mgr", "Reload: out of memory for instances array");
		return -1;
	}

	for (unsigned int i = 0; i < newCfg->instance_count; i++)
	{
		InstanceConfig* nc = &newCfg->instances[i];
		ManagedInstance* ni = &newInstances[i];

		strncpy(ni->name, nc->name, sizeof(ni->name) - 1);
		ni->name[sizeof(ni->name) - 1] = '\0';
		memcpy(&ni->config, nc, sizeof(InstanceConfig));

		/* Find in old instances to transfer state */
		ManagedInstance* oldInst = NULL;
		for (unsigned int j = 0; j < oldCount; j++)
		{
			if (strcmp(oldInstances[j].name, nc->name) == 0)
			{
				oldInst = &oldInstances[j];
				break;
			}
		}

		if (!oldInst)
		{
			/* ── New instance ────────────────────────────────── */
			ni->state = INST_STOPPED;
			ni->hProcess = NULL;
			ni->hJob = NULL;
			ni->pid = 0;
			ni->hHeartbeatPipe = NULL;
			ni->lastHeartbeatMs = 0;
			ni->reconnectAttempts = 0;
			ni->nextReconnectMs = 0;
			ni->viewerCount = 0;
			ni->stopRequested = FALSE;
		}
		else
		{
			/* ── Existing instance – preserve remaining state ── */
			/* (handles already cleaned up by inst_mgr_stop above
			 *  if this instance was removed, disabled, or breaking) */
			ni->state = oldInst->state;
			ni->hProcess = oldInst->hProcess;
			ni->hJob = oldInst->hJob;
			ni->pid = oldInst->pid;
			ni->hHeartbeatPipe = oldInst->hHeartbeatPipe;
			ni->lastHeartbeatMs = oldInst->lastHeartbeatMs;
			ni->reconnectAttempts = oldInst->reconnectAttempts;
			ni->nextReconnectMs = oldInst->nextReconnectMs;
			ni->viewerCount = oldInst->viewerCount;
			ni->stopRequested = oldInst->stopRequested;
		}
	}

	/* ── 5. Swap arrays and config pointer ──────────────────────── */
	mgr->instances = newInstances;
	mgr->instanceCount = newCfg->instance_count;
	mgr->config = newCfg;
	mgr->configPath = configPath;

	LeaveCriticalSection(&mgr->lock);

	/* ── 6. Start new instances and restart breaking changes ────── */
	/* (uses new mgr->instances after the swap so inst_mgr_find works) */
	for (unsigned int i = 0; i < newCfg->instance_count; i++)
	{
		InstanceConfig* nc = &newCfg->instances[i];

		if (!nc->enabled)
			continue;

		/* Determine if this instance needs starting */
		int should_start = 0;

		/* Check if it's a new instance (not in old config) */
		int found_in_old = 0;
		for (unsigned int j = 0; j < oldCount; j++)
		{
			if (strcmp(oldInstances[j].name, nc->name) == 0)
			{
				found_in_old = 1;

				/* Was disabled, now enabled – start */
				if (!oldInstances[j].config.enabled)
				{
					LOG_I("svc_inst_mgr",
					      "Reload: instance '%s' was disabled, "
					      "now enabled – starting",
					      nc->name);
					should_start = 1;
					break;
				}

				/* Check for breaking changes that need restart */
				ManagedInstance* oldInst = &oldInstances[j];
				int is_breaking = 0;

				if (strcmp(oldInst->config.backend_hostname, nc->backend_hostname) != 0 ||
				    oldInst->config.backend_port != nc->backend_port ||
				    strcmp(oldInst->config.backend_username, nc->backend_username) != 0 ||
				    strcmp(oldInst->config.backend_domain, nc->backend_domain) != 0)
				{
					is_breaking = 1;
				}
				if (strcmp(oldInst->config.viewer_bind_address, nc->viewer_bind_address) != 0 ||
				    oldInst->config.viewer_port != nc->viewer_port)
				{
					is_breaking = 1;
				}
				if (oldInst->config.display_monitor_count != nc->display_monitor_count ||
				    oldInst->config.display_monitor_width != nc->display_monitor_width ||
				    oldInst->config.display_monitor_height != nc->display_monitor_height ||
				    oldInst->config.display_color_depth != nc->display_color_depth)
				{
					is_breaking = 1;
				}
				if (oldInst->config.codec_nscodec != nc->codec_nscodec ||
				    oldInst->config.codec_remote_fx != nc->codec_remote_fx ||
				    oldInst->config.codec_graphics_pipeline != nc->codec_graphics_pipeline ||
				    oldInst->config.codec_h264 != nc->codec_h264 ||
				    oldInst->config.codec_avc444 != nc->codec_avc444 ||
				    oldInst->config.codec_avc444v2 != nc->codec_avc444v2 ||
				    oldInst->config.codec_frame_acknowledge != nc->codec_frame_acknowledge)
				{
					is_breaking = 1;
				}
				if (oldInst->config.security_tls_enabled != nc->security_tls_enabled ||
				    oldInst->config.security_nla_enabled != nc->security_nla_enabled ||
				    strcmp(oldInst->config.security_tls_min_version,
				           nc->security_tls_min_version) != 0 ||
				    oldInst->config.security_server_authentication !=
				        nc->security_server_authentication ||
				    oldInst->config.security_ignore_certificate != nc->security_ignore_certificate)
				{
					is_breaking = 1;
				}
				if (strcmp(oldInst->config.backend_password, nc->backend_password) != 0)
				{
					is_breaking = 1;
				}

				if (is_breaking)
				{
					LOG_I("svc_inst_mgr",
					      "Reload: restarting instance '%s' after "
					      "breaking change",
					      nc->name);
					should_start = 1;
				}
				break;
			}
		}

		if (!found_in_old)
		{
			LOG_I("svc_inst_mgr", "Reload: starting new instance '%s'", nc->name);
			should_start = 1;
		}

		if (should_start)
		{
			if (inst_mgr_start(mgr, nc->name) != 0)
			{
				LOG_E("svc_inst_mgr", "Reload: failed to start instance '%s'", nc->name);
			}
		}
	}

	/* ── 7. Free old resources ──────────────────────────────────── */
	free(oldInstances);
	svc_config_free(oldCfg);

	LOG_I("svc_inst_mgr", "Config reloaded successfully (%u instances)", newCfg->instance_count);

	return 0;
}

/* ── inst_mgr_cleanup ──────────────────────────────────────────── */

void
inst_mgr_cleanup(InstanceManager* mgr)
{
	if (!mgr)
		return;

	LOG_I("svc_inst_mgr", "Cleaning up instance manager...");

	/* Stop all running instances */
	inst_mgr_stop_all(mgr);

	EnterCriticalSection(&mgr->lock);

	/* Close any remaining job handles and heartbeat pipes */
	for (unsigned int i = 0; i < mgr->instanceCount; i++)
	{
		ManagedInstance* inst = &mgr->instances[i];

		if (inst->hJob)
		{
			CloseHandle(inst->hJob);
			inst->hJob = NULL;
		}
		if (inst->hHeartbeatPipe)
		{
			CloseHandle(inst->hHeartbeatPipe);
			inst->hHeartbeatPipe = NULL;
		}
		if (inst->hProcess)
		{
			CloseHandle(inst->hProcess);
			inst->hProcess = NULL;
		}
		inst->pid = 0;
		inst->state = INST_STOPPED;
	}

	/* Free the instances array */
	free(mgr->instances);
	mgr->instances = NULL;
	mgr->instanceCount = 0;

	LeaveCriticalSection(&mgr->lock);

	DeleteCriticalSection(&mgr->lock);

	LOG_I("svc_inst_mgr", "Instance manager cleanup complete");
}
