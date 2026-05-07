/**
 * @file svc_service.h
 * @brief OmniRDP service entry point, SCM integration, and context
 *
 * Windows service lifecycle management for OmniRDP-svc.exe.
 * Provides install/uninstall, SCM-controlled service start/stop,
 * and a console-mode equivalent for debugging.
 *
 * Windows-only; no FreeRDP dependency.
 */

#ifndef SVC_SERVICE_H
#define SVC_SERVICE_H

#include <windows.h>

#include "svc_config.h"
#include "svc_instance_mgr.h"
#include "svc_pipe_server.h"

#ifdef __cplusplus
extern "C"
{
#endif

	/**
	 * @brief Service-wide context (singleton)
	 *
	 * Holds all runtime state for the OmniRDP service.
	 * Allocated and initialized by svc_service_start().
	 */
	typedef struct
	{
		/* Service registration */
		char serviceName[256];     /* e.g., "OmniRDP" or "OmniRDP-Prod" */
		char configPath[MAX_PATH]; /* Path to config.ini */
		char exePath[MAX_PATH];    /* Path to OmniRDP.exe */

		/* SCM handles */
		SERVICE_STATUS_HANDLE statusHandle;
		SERVICE_STATUS status;

		/* Instance manager */
		InstanceManager mgr; /* Will be initialized in svc_service_start */
		SvcConfig* config;   /* Current config (owned by service) */

		/* Pipe server for tray app IPC */
		PipeServer pipeServer; /* Named pipe IPC server */

		/* Threading */
		HANDLE hMainThread; /* Handle to the main service thread */
		volatile DWORD mainThreadId;

		/* Shutdown */
		HANDLE hStopEvent; /* Manual-reset event signaled on SERVICE_STOP */
		BOOL shuttingDown;
	} OmniRDPSvcContext;

	/**
	 * @brief Install the service with the SCM
	 *
	 * Registers the service, sets the config path in the registry,
	 * and configures the service to auto-start.
	 *
	 * @param serviceName Display name and service name (e.g., "OmniRDP" or
	 * "OmniRDP-Prod")
	 * @param configPath  Path to config.ini (stored in service parameters)
	 * @return 0 on success, -1 on error
	 */
	int svc_service_install(const char* serviceName, const char* configPath);

	/**
	 * @brief Uninstall the service
	 *
	 * Stops the service if running, then deletes it from the SCM.
	 *
	 * @param serviceName Service name to uninstall
	 * @return 0 on success, -1 on error
	 */
	int svc_service_uninstall(const char* serviceName);

	/**
	 * @brief Start the service (called by SCM via ServiceMain)
	 *
	 * This is the main entry point when running as a Windows Service.
	 * It initializes the instance manager, starts all enabled instances,
	 * and enters the main service loop.
	 *
	 * @param serviceName Service name (may differ from default "OmniRDP")
	 * @param configPath  Path to config.ini
	 * @return 0 on success, non-zero on error
	 */
	int svc_service_start(const char* serviceName, const char* configPath);

	/**
	 * @brief Run the service in console mode (for debugging)
	 *
	 * Same as svc_service_start but without SCM integration.
	 * Runs in the foreground, Ctrl+C to stop.
	 *
	 * @param serviceName Service name
	 * @param configPath  Path to config.ini
	 * @return 0 on success, non-zero on error
	 */
	int svc_service_run_console(const char* serviceName, const char* configPath);

	/**
	 * @brief Write a template config.ini to the specified path
	 *
	 * Creates the directory structure if needed, writes a well-commented
	 * template config file, and sets ACLs.
	 *
	 * @param configPath Path where the template should be written
	 * @return 0 on success, -1 on error
	 */
	int svc_config_write_template(const char* configPath);

#ifdef __cplusplus
}
#endif

#endif /* SVC_SERVICE_H */
