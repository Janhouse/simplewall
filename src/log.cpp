// simplewall
// Copyright (c) 2016-2019 Henry++

#include "global.hpp"

void _app_logerror (LPCWSTR fn, DWORD errcode, LPCWSTR desc, bool is_nopopups)
{
	const time_t current_time = _r_unixtime_now ();

	_r_dbg (fn, errcode, desc);

	if ((current_time - app.ConfigGet (L"ErrorNotificationsTimestamp", time_t (0)).AsLonglong ()) >= app.ConfigGet (L"ErrorNotificationsPeriod", time_t (4)).AsLonglong () && !is_nopopups && app.ConfigGet (L"IsErrorNotificationsEnabled", true).AsBool ()) // check for timeout (sec.)
	{
		app.TrayPopup (app.GetHWND (), UID, nullptr, NIIF_ERROR | (app.ConfigGet (L"IsNotificationsSound", true).AsBool () ? 0 : NIIF_NOSOUND), APP_NAME, app.LocaleString (IDS_STATUS_ERROR, nullptr));
		app.ConfigSet (L"ErrorNotificationsTimestamp", current_time);
	}
}

rstring _app_getlogviewer ()
{
	rstring result = app.ConfigGet (L"LogViewer", LOG_VIEWER_DEFAULT);

	if (result.IsEmpty ())
		return _r_path_expand (LOG_VIEWER_DEFAULT);

	return _r_path_expand (result);
}

bool _app_loginit (bool is_install)
{
	// dropped packets logging (win7+)
	if (!config.hnetevent)
		return false;

	// reset all handles
	_r_fastlock_acquireexclusive (&lock_writelog);

	if (config.hlogfile != nullptr && config.hlogfile != INVALID_HANDLE_VALUE)
	{
		CloseHandle (config.hlogfile);
		config.hlogfile = nullptr;
	}

	_r_fastlock_releaseexclusive (&lock_writelog);

	if (!is_install)
		return true; // already closed

	// check if log enabled
	if (!app.ConfigGet (L"IsLogEnabled", false).AsBool ())
		return false;

	bool result = false;

	const rstring path = _r_path_expand (app.ConfigGet (L"LogPath", LOG_PATH_DEFAULT));

	_r_fastlock_acquireexclusive (&lock_writelog);

	config.hlogfile = CreateFile (path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

	if (config.hlogfile == INVALID_HANDLE_VALUE)
	{
		_app_logerror (L"CreateFile", GetLastError (), path, false);
	}
	else
	{
		if (GetLastError () != ERROR_ALREADY_EXISTS)
		{
			DWORD written = 0;
			static const BYTE bom[] = {0xFF, 0xFE};

			WriteFile (config.hlogfile, bom, sizeof (bom), &written, nullptr); // write utf-16 le byte order mask
		}
		else
		{
			_app_logchecklimit ();

			_r_fs_setpos (config.hlogfile, 0, FILE_END);
		}

		result = true;
	}

	_r_fastlock_releaseexclusive (&lock_writelog);

	return result;
}

void _app_logwrite (PITEM_LOG ptr_log)
{
	if (!ptr_log || !config.hlogfile || config.hlogfile == INVALID_HANDLE_VALUE)
		return;

	// parse path
	rstring path;
	{
		_r_fastlock_acquireshared (&lock_access);
		PR_OBJECT ptr_app_object = _app_getappitem (ptr_log->app_hash);
		_r_fastlock_releaseshared (&lock_access);

		if (ptr_app_object)
		{
			PITEM_APP ptr_app = (PITEM_APP)ptr_app_object->pdata;

			if (ptr_app)
			{
				if (ptr_app->type == DataAppUWP || ptr_app->type == DataAppService)
				{
					if (ptr_app->real_path && ptr_app->real_path[0])
						path = ptr_app->real_path;

					else if (ptr_app->display_name && ptr_app->display_name[0])
						path = ptr_app->display_name;
				}
				else if (ptr_app->original_path && ptr_app->original_path[0])
				{
					path = ptr_app->original_path;
				}
			}

			_r_obj_dereference (ptr_app_object, &_app_dereferenceapp);
		}
	}

	// parse filter name
	rstring filter_name;
	{
		if ((ptr_log->provider_name && ptr_log->provider_name[0]) && (ptr_log->filter_name && ptr_log->filter_name[0]))
			filter_name.Format (L"%s\\%s", ptr_log->provider_name, ptr_log->filter_name);

		else if (ptr_log->filter_name && ptr_log->filter_name[0])
			filter_name = ptr_log->filter_name;
	}

	// parse direction
	rstring direction;
	{
		if (ptr_log->direction == FWP_DIRECTION_INBOUND)
			direction = SZ_LOG_DIRECTION_IN;

		else if (ptr_log->direction == FWP_DIRECTION_OUTBOUND)
			direction = SZ_LOG_DIRECTION_OUT;

		if (ptr_log->is_loopback)
			direction.Append (SZ_LOG_DIRECTION_LOOPBACK);
	}

	LPWSTR addr_fmt = nullptr;
	_app_formataddress (ptr_log->af, 0, &ptr_log->addr, 0, &addr_fmt, FMTADDR_RESOLVE_HOST);

	rstring buffer;
	buffer.Format (L"\"%s\"%c\"%s\"%c\"%s\"%c\"%s\"%c\"%s\"%c\"%s\"%c\"%s\"%c\"#%" PRIu64 L"\"%c\"%s\"%c\"%s\"\r\n",
				   _r_fmt_date (ptr_log->date, FDTF_SHORTDATE | FDTF_LONGTIME).GetString (),
				   DIVIDER_CSV,
				   ptr_log->username && ptr_log->username[0] ? ptr_log->username : SZ_EMPTY,
				   DIVIDER_CSV,
				   path.IsEmpty () ? SZ_EMPTY : path.GetString (),
				   DIVIDER_CSV,
				   addr_fmt && addr_fmt[0] ? addr_fmt : SZ_EMPTY,
				   DIVIDER_CSV,
				   ptr_log->port ? _r_fmt (L"%" PRIu16 L" (%s)", ptr_log->port, _app_getservicename (ptr_log->port).GetString ()).GetString () : SZ_EMPTY,
				   DIVIDER_CSV,
				   _app_getprotoname (ptr_log->protocol, ptr_log->af).GetString (),
				   DIVIDER_CSV,
				   filter_name.IsEmpty () ? SZ_EMPTY : filter_name.GetString (),
				   DIVIDER_CSV,
				   ptr_log->filter_id,
				   DIVIDER_CSV,
				   direction.IsEmpty () ? SZ_EMPTY : direction.GetString (),
				   DIVIDER_CSV,
				   (ptr_log->is_allow ? SZ_LOG_ALLOW : SZ_LOG_BLOCK)
	);

	SAFE_DELETE_ARRAY (addr_fmt);

	_r_fastlock_acquireexclusive (&lock_writelog);

	_app_logchecklimit ();

	DWORD written = 0;
	WriteFile (config.hlogfile, buffer.GetString (), DWORD (buffer.GetLength () * sizeof (WCHAR)), &written, nullptr);

	_r_fastlock_releaseexclusive (&lock_writelog);
}

bool _app_logchecklimit ()
{
	const DWORD limit = app.ConfigGet (L"LogSizeLimitKb", LOG_SIZE_LIMIT_DEFAULT).AsUlong ();

	if (!limit || !config.hlogfile || config.hlogfile == INVALID_HANDLE_VALUE)
		return false;

	if (_r_fs_size (config.hlogfile) >= (limit * _R_BYTESIZE_KB))
	{
		_app_logclear ();

		return true;
	}

	return false;
}

void _app_logclear ()
{
	const rstring path = _r_path_expand (app.ConfigGet (L"LogPath", LOG_PATH_DEFAULT));

	if (config.hlogfile != nullptr && config.hlogfile != INVALID_HANDLE_VALUE)
	{
		_r_fs_setpos (config.hlogfile, 2, FILE_BEGIN);

		SetEndOfFile (config.hlogfile);
		FlushFileBuffers (config.hlogfile);
	}
	else
	{
		_r_fs_delete (path, false);
	}

	_r_fs_delete (_r_fmt (L"%s.bak", path.GetString ()), false);
}

bool _wfp_logsubscribe ()
{
	if (!config.hengine)
		return false;

	bool result = false;

	if (config.hnetevent)
	{
		result = true;
	}
	else
	{
		const HMODULE hlib = LoadLibrary (L"fwpuclnt.dll");

		if (!hlib)
		{
			_app_logerror (L"LoadLibrary", GetLastError (), L"fwpuclnt.dll", false);
		}
		else
		{
			typedef DWORD (WINAPI * FWPMNES5) (HANDLE, const FWPM_NET_EVENT_SUBSCRIPTION0 *, FWPM_NET_EVENT_CALLBACK4, LPVOID, LPHANDLE); // win10new+
			typedef DWORD (WINAPI * FWPMNES4) (HANDLE, const FWPM_NET_EVENT_SUBSCRIPTION0 *, FWPM_NET_EVENT_CALLBACK4, LPVOID, LPHANDLE); // win10rs5+
			typedef DWORD (WINAPI * FWPMNES3) (HANDLE, const FWPM_NET_EVENT_SUBSCRIPTION0 *, FWPM_NET_EVENT_CALLBACK3, LPVOID, LPHANDLE); // win10rs4+
			typedef DWORD (WINAPI * FWPMNES2) (HANDLE, const FWPM_NET_EVENT_SUBSCRIPTION0 *, FWPM_NET_EVENT_CALLBACK2, LPVOID, LPHANDLE); // win10+
			typedef DWORD (WINAPI * FWPMNES1) (HANDLE, const FWPM_NET_EVENT_SUBSCRIPTION0 *, FWPM_NET_EVENT_CALLBACK1, LPVOID, LPHANDLE); // win8+
			typedef DWORD (WINAPI * FWPMNES0) (HANDLE, const FWPM_NET_EVENT_SUBSCRIPTION0 *, FWPM_NET_EVENT_CALLBACK0, LPVOID, LPHANDLE); // win7+

			const FWPMNES5 _FwpmNetEventSubscribe5 = (FWPMNES5)GetProcAddress (hlib, "FwpmNetEventSubscribe5"); // win10new+
			const FWPMNES4 _FwpmNetEventSubscribe4 = (FWPMNES4)GetProcAddress (hlib, "FwpmNetEventSubscribe4"); // win10rs5+
			const FWPMNES3 _FwpmNetEventSubscribe3 = (FWPMNES3)GetProcAddress (hlib, "FwpmNetEventSubscribe3"); // win10rs4+
			const FWPMNES2 _FwpmNetEventSubscribe2 = (FWPMNES2)GetProcAddress (hlib, "FwpmNetEventSubscribe2"); // win10+
			const FWPMNES1 _FwpmNetEventSubscribe1 = (FWPMNES1)GetProcAddress (hlib, "FwpmNetEventSubscribe1"); // win8+
			const FWPMNES0 _FwpmNetEventSubscribe0 = (FWPMNES0)GetProcAddress (hlib, "FwpmNetEventSubscribe0"); // win7+

			if (!_FwpmNetEventSubscribe5 && !_FwpmNetEventSubscribe4 && !_FwpmNetEventSubscribe3 && !_FwpmNetEventSubscribe2 && !_FwpmNetEventSubscribe1 && !_FwpmNetEventSubscribe0)
			{
				_app_logerror (L"GetProcAddress", GetLastError (), L"FwpmNetEventSubscribe", false);
			}
			else
			{
				FWPM_NET_EVENT_SUBSCRIPTION subscription;
				FWPM_NET_EVENT_ENUM_TEMPLATE enum_template;

				SecureZeroMemory (&subscription, sizeof (subscription));
				SecureZeroMemory (&enum_template, sizeof (enum_template));

				if (config.psession)
					CopyMemory (&subscription.sessionKey, config.psession, sizeof (GUID));

				subscription.enumTemplate = &enum_template;

				DWORD rc = 0;

				if (_FwpmNetEventSubscribe5)
					rc = _FwpmNetEventSubscribe5 (config.hengine, &subscription, &_wfp_logcallback4, nullptr, &config.hnetevent); // win10new+

				else if (_FwpmNetEventSubscribe4)
					rc = _FwpmNetEventSubscribe4 (config.hengine, &subscription, &_wfp_logcallback4, nullptr, &config.hnetevent); // win10rs5+

				else if (_FwpmNetEventSubscribe3)
					rc = _FwpmNetEventSubscribe3 (config.hengine, &subscription, &_wfp_logcallback3, nullptr, &config.hnetevent); // win10rs4+

				else if (_FwpmNetEventSubscribe2)
					rc = _FwpmNetEventSubscribe2 (config.hengine, &subscription, &_wfp_logcallback2, nullptr, &config.hnetevent); // win10+

				else if (_FwpmNetEventSubscribe1)
					rc = _FwpmNetEventSubscribe1 (config.hengine, &subscription, &_wfp_logcallback1, nullptr, &config.hnetevent); // win8+

				else if (_FwpmNetEventSubscribe0)
					rc = _FwpmNetEventSubscribe0 (config.hengine, &subscription, &_wfp_logcallback0, nullptr, &config.hnetevent); // win7+

				if (rc != ERROR_SUCCESS)
				{
					_app_logerror (L"FwpmNetEventSubscribe", rc, nullptr, false);
				}
				else
				{
					_app_loginit (true); // create log file
					result = true;
				}
			}

			FreeLibrary (hlib);
		}
	}

	return result;
}

bool _wfp_logunsubscribe ()
{
	bool result = false;

	_app_loginit (false); // destroy log file handle if present

	if (config.hnetevent)
	{
		const HMODULE hlib = LoadLibrary (L"fwpuclnt.dll");

		if (hlib)
		{
			typedef DWORD (WINAPI * FWPMNEU) (HANDLE, HANDLE); // FwpmNetEventUnsubscribe0

			const FWPMNEU _FwpmNetEventUnsubscribe = (FWPMNEU)GetProcAddress (hlib, "FwpmNetEventUnsubscribe0");

			if (_FwpmNetEventUnsubscribe)
			{
				const DWORD rc = _FwpmNetEventUnsubscribe (config.hengine, config.hnetevent);

				if (rc == ERROR_SUCCESS)
				{
					config.hnetevent = nullptr;
					result = true;
				}
			}

			FreeLibrary (hlib);
		}
	}

	return result;
}

void CALLBACK _wfp_logcallback (UINT32 flags, FILETIME const *pft, UINT8 *app_id, SID * package_id, SID * user_id, UINT8 proto, FWP_IP_VERSION ipver, UINT32 remote_addr4, FWP_BYTE_ARRAY16 const *remote_addr6, UINT16 remoteport, UINT16 layer_id, UINT64 filter_id, UINT32 direction, bool is_allow, bool is_loopback)
{
	if (!filter_id || !layer_id || _wfp_isfiltersapplying () || (is_allow && app.ConfigGet (L"IsExcludeClassifyAllow", true).AsBool ()))
		return;

	// set allowed directions directions
	switch (direction)
	{
		case FWP_DIRECTION_IN:
		case FWP_DIRECTION_INBOUND:
		case FWP_DIRECTION_OUT:
		case FWP_DIRECTION_OUTBOUND:
		{
			break;
		}

		default:
		{
			return;
		}
	}

	// do not parse when tcp connection has been established, or when non-tcp traffic has been authorized
	{
		FWPM_LAYER *layer = nullptr;

		if (FwpmLayerGetById (config.hengine, layer_id, &layer) == ERROR_SUCCESS && layer)
		{
			if (memcmp (&layer->layerKey, &FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4, sizeof (GUID)) == 0 || memcmp (&layer->layerKey, &FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6, sizeof (GUID)) == 0)
			{
				FwpmFreeMemory ((void **)& layer);
				return;
			}

			FwpmFreeMemory ((void **)& layer);
		}
	}

	// get filter information
	bool is_myprovider = false;

	rstring filter_name;
	rstring provider_name;

	UINT8 filter_weight = 0;

	{
		FWPM_FILTER *ptr_filter = nullptr;
		FWPM_PROVIDER *ptr_provider = nullptr;

		if (FwpmFilterGetById (config.hengine, filter_id, &ptr_filter) == ERROR_SUCCESS && ptr_filter)
		{
			filter_name = ptr_filter->displayData.name ? ptr_filter->displayData.name : ptr_filter->displayData.description;

			if (ptr_filter->weight.type == FWP_UINT8)
				filter_weight = ptr_filter->weight.uint8;

			if (ptr_filter->providerKey)
			{
				if (memcmp (ptr_filter->providerKey, &GUID_WfpProvider, sizeof (GUID)) == 0)
					is_myprovider = true;

				if (FwpmProviderGetByKey (config.hengine, ptr_filter->providerKey, &ptr_provider) == ERROR_SUCCESS && ptr_provider)
					provider_name = ptr_provider->displayData.name ? ptr_provider->displayData.name : ptr_provider->displayData.description;
			}
		}

		if (ptr_filter)
			FwpmFreeMemory ((void **)& ptr_filter);

		if (ptr_provider)
			FwpmFreeMemory ((void **)& ptr_provider);

		// prevent filter "not found" items
		if (filter_name.IsEmpty () && provider_name.IsEmpty ())
			return;
	}

	PITEM_LIST_ENTRY ptr_entry = (PITEM_LIST_ENTRY)_aligned_malloc (sizeof (ITEM_LIST_ENTRY), MEMORY_ALLOCATION_ALIGNMENT);

	if (ptr_entry)
	{
		PITEM_LOG ptr_log = new ITEM_LOG;

		// get package id (win8+)
		rstring sidstring;

		if ((flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) != 0 && package_id)
		{
			sidstring = _r_str_fromsid (package_id);

			if (sidstring.IsEmpty () || !_app_item_get (DataAppUWP, sidstring.Hash (), nullptr, nullptr, nullptr))
				sidstring.Clear ();
		}

		// copy converted nt device path into win32
		if ((flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) != 0 && !sidstring.IsEmpty ())
		{
			_r_str_alloc (&ptr_log->path, sidstring.GetLength (), sidstring);

			ptr_log->app_hash = sidstring.Hash ();
		}
		else if ((flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) != 0 && app_id)
		{
			const rstring path = _r_path_dospathfromnt (LPCWSTR (app_id));

			_r_str_alloc (&ptr_log->path, path.GetLength (), path);

			ptr_log->app_hash = path.Hash ();

			_app_applycasestyle (ptr_log->path, path.GetLength ()); // apply case-style
		}
		else
		{
			ptr_log->app_hash = 0;
		}

		// copy date and time
		if (pft)
			ptr_log->date = _r_unixtime_from_filetime (pft);

		// get username information
		if ((flags & FWPM_NET_EVENT_FLAG_USER_ID_SET) != 0 && user_id)
		{
			SID_NAME_USE sid_type = SidTypeInvalid;

			WCHAR username[MAX_PATH] = {0};
			WCHAR domain[MAX_PATH] = {0};

			DWORD length1 = _countof (username);
			DWORD length2 = _countof (domain);

			if (LookupAccountSid (nullptr, user_id, username, &length1, domain, &length2, &sid_type))
			{
				rstring userstring;
				userstring.Format (L"%s\\%s", domain, username);

				_r_str_alloc (&ptr_log->username, userstring.GetLength (), userstring);
			}
		}

		// indicates the direction of the packet transmission
		switch (direction)
		{
			case FWP_DIRECTION_IN:
			case FWP_DIRECTION_INBOUND:
			{
				ptr_log->direction = FWP_DIRECTION_INBOUND;
				break;
			}

			case FWP_DIRECTION_OUT:
			case FWP_DIRECTION_OUTBOUND:
			{
				ptr_log->direction = FWP_DIRECTION_OUTBOUND;
				break;
			}
		}

		// destination
		if ((flags & FWPM_NET_EVENT_FLAG_IP_VERSION_SET) != 0)
		{
			if (ipver == FWP_IP_VERSION_V4)
			{
				ptr_log->af = AF_INET;

				// remote address
				if ((flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) != 0 && remote_addr4)
					ptr_log->addr.S_un.S_addr = ntohl (remote_addr4);

			}
			else if (ipver == FWP_IP_VERSION_V6)
			{
				ptr_log->af = AF_INET6;

				// remote address
				if ((flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) != 0 && remote_addr6)
					CopyMemory (ptr_log->addr6.u.Byte, remote_addr6->byteArray16, FWP_V6_ADDR_SIZE);
			}

			if ((flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) != 0)
				ptr_log->port = remoteport;
		}

		// protocol
		if ((flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) != 0)
			ptr_log->protocol = proto;

		// indicates FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW state
		ptr_log->is_allow = is_allow;

		// indicates whether the packet originated from (or was heading to) the loopback adapter
		ptr_log->is_loopback = is_loopback;

		// set filter information
		ptr_log->filter_id = filter_id;

		ptr_log->is_myprovider = is_myprovider;

		_r_str_alloc (&ptr_log->filter_name, filter_name.GetLength (), filter_name);
		_r_str_alloc (&ptr_log->provider_name, provider_name.GetLength (), provider_name);

		ptr_log->is_blocklist = (filter_weight == FILTER_WEIGHT_BLOCKLIST);
		ptr_log->is_system = (filter_weight == FILTER_WEIGHT_HIGHEST) || (filter_weight == FILTER_WEIGHT_HIGHEST_IMPORTANT);
		ptr_log->is_custom = (filter_weight == FILTER_WEIGHT_CUSTOM) || (filter_weight == FILTER_WEIGHT_CUSTOM_BLOCK);

		// push into a slist
		{
			ptr_entry->Body = _r_obj_allocate (ptr_log);

			InterlockedPushEntrySList (&log_stack.ListHead, &ptr_entry->ListEntry);
			const LONG new_item_count = InterlockedIncrement (&log_stack.item_count);

			// check if thread has been terminated
			const LONG thread_count = InterlockedCompareExchange (&log_stack.thread_count, 0, 0);

			if (!_r_fastlock_islocked (&lock_logthread) || (_r_fastlock_islocked (&lock_logbusy) && new_item_count >= NOTIFY_LIMIT_POOL_SIZE && thread_count >= 1 && thread_count < std::clamp (app.ConfigGet (L"LogThreadsLimit", NOTIFY_LIMIT_THREAD_COUNT).AsInt (), 1, 8)))
			{
				_r_fastlock_acquireexclusive (&lock_threadpool);
				_app_freethreadpool (&threads_pool);
				_r_fastlock_releaseexclusive (&lock_threadpool);

				const HANDLE hthread = _r_createthread (&LogThread, app.GetHWND (), true, THREAD_PRIORITY_BELOW_NORMAL);

				if (hthread)
				{
					InterlockedIncrement (&log_stack.thread_count);

					_r_fastlock_acquireexclusive (&lock_threadpool);
					threads_pool.push_back (hthread);
					_r_fastlock_releaseexclusive (&lock_threadpool);

					ResumeThread (hthread);
				}
			}
		}
	}
}

// win7+ callback
void CALLBACK _wfp_logcallback0 (LPVOID, const FWPM_NET_EVENT1 * pEvent)
{
	if (pEvent)
	{
		UINT16 layer_id;
		UINT64 filter_id;
		UINT32 direction;
		bool is_loopback;

		if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && pEvent->classifyDrop)
		{
			layer_id = pEvent->classifyDrop->layerId;
			filter_id = pEvent->classifyDrop->filterId;
			direction = pEvent->classifyDrop->msFwpDirection;
			is_loopback = pEvent->classifyDrop->isLoopback;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && pEvent->ipsecDrop)
		{
			layer_id = pEvent->ipsecDrop->layerId;
			filter_id = pEvent->ipsecDrop->filterId;
			direction = pEvent->ipsecDrop->direction;
			is_loopback = false;
		}
		else
		{
			return;
		}

		_wfp_logcallback (pEvent->header.flags, &pEvent->header.timeStamp, pEvent->header.appId.data, nullptr, pEvent->header.userId, pEvent->header.ipProtocol, pEvent->header.ipVersion, pEvent->header.remoteAddrV4, &pEvent->header.remoteAddrV6, pEvent->header.remotePort, layer_id, filter_id, direction, false, is_loopback);
	}
}

// win8+ callback
void CALLBACK _wfp_logcallback1 (LPVOID, const FWPM_NET_EVENT2 * pEvent)
{
	if (pEvent)
	{
		UINT16 layer_id;
		UINT64 filter_id;
		UINT32 direction;
		bool is_loopback;
		bool is_allow = false;

		if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && pEvent->classifyDrop)
		{
			layer_id = pEvent->classifyDrop->layerId;
			filter_id = pEvent->classifyDrop->filterId;
			direction = pEvent->classifyDrop->msFwpDirection;
			is_loopback = pEvent->classifyDrop->isLoopback;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && pEvent->ipsecDrop)
		{
			layer_id = pEvent->ipsecDrop->layerId;
			filter_id = pEvent->ipsecDrop->filterId;
			direction = pEvent->ipsecDrop->direction;
			is_loopback = false;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW && pEvent->classifyAllow)
		{
			layer_id = pEvent->classifyAllow->layerId;
			filter_id = pEvent->classifyAllow->filterId;
			direction = pEvent->classifyAllow->msFwpDirection;
			is_loopback = pEvent->classifyAllow->isLoopback;

			is_allow = true;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC && pEvent->classifyDropMac)
		{
			layer_id = pEvent->classifyDropMac->layerId;
			filter_id = pEvent->classifyDropMac->filterId;
			direction = pEvent->classifyDropMac->msFwpDirection;
			is_loopback = pEvent->classifyDropMac->isLoopback;
		}
		else
		{
			return;
		}

		_wfp_logcallback (pEvent->header.flags, &pEvent->header.timeStamp, pEvent->header.appId.data, pEvent->header.packageSid, pEvent->header.userId, pEvent->header.ipProtocol, pEvent->header.ipVersion, pEvent->header.remoteAddrV4, &pEvent->header.remoteAddrV6, pEvent->header.remotePort, layer_id, filter_id, direction, is_allow, is_loopback);
	}
}

// win10+ callback
void CALLBACK _wfp_logcallback2 (LPVOID, const FWPM_NET_EVENT3 * pEvent)
{
	if (pEvent)
	{
		UINT16 layer_id;
		UINT64 filter_id;
		UINT32 direction;
		bool is_loopback;
		bool is_allow = false;

		if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && pEvent->classifyDrop)
		{
			layer_id = pEvent->classifyDrop->layerId;
			filter_id = pEvent->classifyDrop->filterId;
			direction = pEvent->classifyDrop->msFwpDirection;
			is_loopback = pEvent->classifyDrop->isLoopback;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && pEvent->ipsecDrop)
		{
			layer_id = pEvent->ipsecDrop->layerId;
			filter_id = pEvent->ipsecDrop->filterId;
			direction = pEvent->ipsecDrop->direction;
			is_loopback = false;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW && pEvent->classifyAllow)
		{
			layer_id = pEvent->classifyAllow->layerId;
			filter_id = pEvent->classifyAllow->filterId;
			direction = pEvent->classifyAllow->msFwpDirection;
			is_loopback = pEvent->classifyAllow->isLoopback;

			is_allow = true;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC && pEvent->classifyDropMac)
		{
			layer_id = pEvent->classifyDropMac->layerId;
			filter_id = pEvent->classifyDropMac->filterId;
			direction = pEvent->classifyDropMac->msFwpDirection;
			is_loopback = pEvent->classifyDropMac->isLoopback;
		}
		else
		{
			return;
		}

		_wfp_logcallback (pEvent->header.flags, &pEvent->header.timeStamp, pEvent->header.appId.data, pEvent->header.packageSid, pEvent->header.userId, pEvent->header.ipProtocol, pEvent->header.ipVersion, pEvent->header.remoteAddrV4, &pEvent->header.remoteAddrV6, pEvent->header.remotePort, layer_id, filter_id, direction, is_allow, is_loopback);
	}
}

// win10rs4+ callback
void CALLBACK _wfp_logcallback3 (LPVOID, const FWPM_NET_EVENT4 * pEvent)
{
	if (pEvent)
	{
		UINT16 layer_id;
		UINT64 filter_id;
		UINT32 direction;
		bool is_loopback;
		bool is_allow = false;

		if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && pEvent->classifyDrop)
		{
			layer_id = pEvent->classifyDrop->layerId;
			filter_id = pEvent->classifyDrop->filterId;
			direction = pEvent->classifyDrop->msFwpDirection;
			is_loopback = pEvent->classifyDrop->isLoopback;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && pEvent->ipsecDrop)
		{
			layer_id = pEvent->ipsecDrop->layerId;
			filter_id = pEvent->ipsecDrop->filterId;
			direction = pEvent->ipsecDrop->direction;
			is_loopback = false;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW && pEvent->classifyAllow)
		{
			layer_id = pEvent->classifyAllow->layerId;
			filter_id = pEvent->classifyAllow->filterId;
			direction = pEvent->classifyAllow->msFwpDirection;
			is_loopback = pEvent->classifyAllow->isLoopback;

			is_allow = true;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC && pEvent->classifyDropMac)
		{
			layer_id = pEvent->classifyDropMac->layerId;
			filter_id = pEvent->classifyDropMac->filterId;
			direction = pEvent->classifyDropMac->msFwpDirection;
			is_loopback = pEvent->classifyDropMac->isLoopback;
		}
		else
		{
			return;
		}

		_wfp_logcallback (pEvent->header.flags, &pEvent->header.timeStamp, pEvent->header.appId.data, pEvent->header.packageSid, pEvent->header.userId, pEvent->header.ipProtocol, pEvent->header.ipVersion, pEvent->header.remoteAddrV4, &pEvent->header.remoteAddrV6, pEvent->header.remotePort, layer_id, filter_id, direction, is_allow, is_loopback);
	}
}

// win10rs5+ callback
void CALLBACK _wfp_logcallback4 (LPVOID, const FWPM_NET_EVENT5 * pEvent)
{
	if (pEvent)
	{
		UINT16 layer_id;
		UINT64 filter_id;
		UINT32 direction;
		bool is_loopback;
		bool is_allow = false;

		if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && pEvent->classifyDrop)
		{
			layer_id = pEvent->classifyDrop->layerId;
			filter_id = pEvent->classifyDrop->filterId;
			direction = pEvent->classifyDrop->msFwpDirection;
			is_loopback = pEvent->classifyDrop->isLoopback;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && pEvent->ipsecDrop)
		{
			layer_id = pEvent->ipsecDrop->layerId;
			filter_id = pEvent->ipsecDrop->filterId;
			direction = pEvent->ipsecDrop->direction;
			is_loopback = false;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW && pEvent->classifyAllow)
		{
			layer_id = pEvent->classifyAllow->layerId;
			filter_id = pEvent->classifyAllow->filterId;
			direction = pEvent->classifyAllow->msFwpDirection;
			is_loopback = pEvent->classifyAllow->isLoopback;

			is_allow = true;
		}
		else if (pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC && pEvent->classifyDropMac)
		{
			layer_id = pEvent->classifyDropMac->layerId;
			filter_id = pEvent->classifyDropMac->filterId;
			direction = pEvent->classifyDropMac->msFwpDirection;
			is_loopback = pEvent->classifyDropMac->isLoopback;
		}
		else
		{
			return;
		}

		_wfp_logcallback (pEvent->header.flags, &pEvent->header.timeStamp, pEvent->header.appId.data, pEvent->header.packageSid, pEvent->header.userId, pEvent->header.ipProtocol, pEvent->header.ipVersion, pEvent->header.remoteAddrV4, &pEvent->header.remoteAddrV6, pEvent->header.remotePort, layer_id, filter_id, direction, is_allow, is_loopback);
	}
}

UINT WINAPI LogThread (LPVOID lparam)
{
	const HWND hwnd = (HWND)lparam;

	_r_fastlock_acquireshared (&lock_logthread);

	while (true)
	{
		const PSLIST_ENTRY listEntry = InterlockedPopEntrySList (&log_stack.ListHead);

		if (!listEntry)
			break;

		InterlockedDecrement (&log_stack.item_count);

		PITEM_LIST_ENTRY ptr_entry = CONTAINING_RECORD (listEntry, ITEM_LIST_ENTRY, ListEntry);
		PR_OBJECT ptr_log_object = ptr_entry->Body;

		_aligned_free (ptr_entry);

		if (!ptr_log_object)
			continue;

		PITEM_LOG ptr_log = (PITEM_LOG)ptr_log_object->pdata;

		if (!ptr_log)
		{
			_r_obj_dereference (ptr_log_object, &_app_dereferencelog);
			continue;
		}

		const bool is_logenabled = app.ConfigGet (L"IsLogEnabled", false).AsBool ();
		const bool is_notificationenabled = app.ConfigGet (L"IsNotificationsEnabled", true).AsBool ();

		// apps collector
		_r_fastlock_acquireshared (&lock_access);
		const bool is_notexist = ptr_log->app_hash && ptr_log->path && ptr_log->path[0] && !ptr_log->is_allow && !_app_isappfound (ptr_log->app_hash);
		_r_fastlock_releaseshared (&lock_access);

		if (is_notexist)
		{
			_r_fastlock_acquireshared (&lock_logbusy);

			_r_fastlock_acquireexclusive (&lock_access);
			const size_t app_hash = _app_addapplication (hwnd, ptr_log->path, 0, 0, 0, false, false, true);
			_r_fastlock_releaseexclusive (&lock_access);

			_r_fastlock_releaseshared (&lock_logbusy);

			UINT app_listview_id = 0;

			if (_app_getappinfo (app_hash, InfoListviewId, &app_listview_id, sizeof (app_listview_id)) && app_listview_id == _app_gettab_id (hwnd))
				_app_listviewsort (hwnd, app_listview_id);

			_app_refreshstatus (hwnd);
			_app_profile_save ();
		}

		if ((is_logenabled || is_notificationenabled) && (!(ptr_log->is_system && app.ConfigGet (L"IsExcludeStealth", true).AsBool ())))
		{
			_r_fastlock_acquireshared (&lock_logbusy);
			_app_formataddress (ptr_log->af, ptr_log->protocol, &ptr_log->addr, 0, &ptr_log->addr_fmt, FMTADDR_USE_PROTOCOL | FMTADDR_RESOLVE_HOST);
			_r_fastlock_releaseshared (&lock_logbusy);

			// write log to a file
			if (is_logenabled)
				_app_logwrite (ptr_log);

			// show notification (only for my own provider and file is present)
			if (is_notificationenabled && ptr_log->app_hash && !ptr_log->is_allow && ptr_log->is_myprovider)
			{
				if (!(ptr_log->is_blocklist && app.ConfigGet (L"IsExcludeBlocklist", true).AsBool ()) && !(ptr_log->is_custom && app.ConfigGet (L"IsExcludeCustomRules", true).AsBool ()))
				{
					_r_fastlock_acquireshared (&lock_access);
					PR_OBJECT ptr_app_object = _app_getappitem (ptr_log->app_hash);
					_r_fastlock_releaseshared (&lock_access);

					if (ptr_app_object)
					{
						PITEM_APP ptr_app = (PITEM_APP)ptr_app_object->pdata;

						if (ptr_app)
						{
							if (!ptr_app->is_silent)
								_app_notifyadd (config.hnotification, _r_obj_reference (ptr_log_object), ptr_app);
						}

						_r_obj_dereference (ptr_app_object, &_app_dereferenceapp);
					}
				}
			}
		}

		_r_obj_dereference (ptr_log_object, &_app_dereferencelog);
	}

	_r_fastlock_releaseshared (&lock_logthread);

	InterlockedDecrement (&log_stack.thread_count);

	_endthreadex (0);

	return ERROR_SUCCESS;
}
