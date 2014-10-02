/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2014 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */

// This file is included inside the RequestHandler class.

protected:

struct RequestAnalysis {
	const LString *flags;
	ServerKit::HeaderTable::Cell *appGroupNameCell;
	bool unionStationSupport;
};

virtual void
onRequestBegin(Client *client, Request *req) {
	ParentClass::onRequestBegin(client, req);

	{
		// Perform hash table operations as close to header parsing as possible,
		// and localize them as much as possible, for better CPU caching.
		RequestAnalysis analysis;
		analysis.flags = req->secureHeaders.lookup(FLAGS);
		analysis.appGroupNameCell = singleAppMode
			? NULL
			: req->secureHeaders.lookupCell(PASSENGER_APP_GROUP_NAME);
		analysis.unionStationSupport = unionStationCore != NULL
			&& getBoolOption(req, UNION_STATION_SUPPORT, false);
		req->stickySession = getBoolOption(req, PASSENGER_STICKY_SESSIONS, false);
		req->host = req->headers.lookup(HTTP_HOST);

		/***************/

		req->maxRequestTime = getUIntOption(req, PASSENGER_MAX_REQUEST_TIME, false);

		/***************/

		SKC_TRACE(client, 2, "Initiating request");
		req->startedAt = ev_now(getLoop());
		req->bodyChannel.stop();

		initializeFlags(client, req, analysis);
		initializePoolOptions(client, req, analysis);
		if (req->ended()) {
			return;
		}
		initializeUnionStation(client, req, analysis);
		if (req->ended()) {
			return;
		}
		setStickySessionId(client, req);
	}

	if (!req->hasBody() || !req->requestBodyBuffering) {
		req->requestBodyBuffering = false;
		checkoutSession(client, req);
	} else {
		beginBufferingBody(client, req);
	}
}

virtual bool
supportsUpgrade(Client *client, Request *req) {
	return true;
}

private:

void
initializeFlags(Client *client, Request *req, RequestAnalysis &analysis) {
	if (analysis.flags != NULL) {
		const LString::Part *part = analysis.flags->start;
		while (part != NULL) {
			const char *data = part->data;
			const char *end  = part->data + part->size;
			while (data < end) {
				switch (*data) {
				case 'D':
					req->dechunkResponse = true;
					break;
				case 'B':
					req->requestBodyBuffering = true;
					break;
				case 'S':
					req->https = true;
					break;
				case 'C':
					req->strip100ContinueHeader = true;
					break;
				default:
					break;
				}
				data++;
			}
			part = part->next;
		}

		if (OXT_UNLIKELY(getLogLevel() >= LVL_DEBUG2)) {
			if (req->dechunkResponse) {
				SKC_TRACE(client, 2, "Dechunk flag detected");
			}
			if (req->requestBodyBuffering) {
				SKC_TRACE(client, 2, "Request body buffering enabled");
			}
			if (req->https) {
				SKC_TRACE(client, 2, "HTTPS flag detected");
			}
			if (req->strip100ContinueHeader) {
				SKC_TRACE(client, 2, "Stripping 100 Continue header");
			}
		}
	}
}

void
initializePoolOptions(Client *client, Request *req, RequestAnalysis &analysis) {
	boost::shared_ptr<Options> *options;

	if (singleAppMode) {
		P_ASSERT_EQ(poolOptionsCache.size(), 1);
		poolOptionsCache.lookupRandom(NULL, &options);
		req->options = **options;
	} else {
		ServerKit::HeaderTable::Cell *appGroupNameCell = analysis.appGroupNameCell;
		if (appGroupNameCell != NULL && appGroupNameCell->header->val.size > 0) {
			const LString *appGroupName = psg_lstr_make_contiguous(
				&appGroupNameCell->header->val,
				req->pool);

			poolOptionsCache.lookup(HashedStaticString(appGroupName->start->data,
				appGroupName->size), &options);

			if (options != NULL) {
				req->options = **options;
			} else {
				createNewPoolOptions(client, req, appGroupName);
			}
		} else {
			disconnectWithError(&client, "the !~PASSENGER_APP_GROUP_NAME header must be set");
			return;
		}
	}

	if (!req->ended()) {
		fillPoolOption(req, req->options.maxRequests, PASSENGER_MAX_REQUESTS);
	}
}

void
fillPoolOptionsFromAgentsOptions(Options &options) {
	options.ruby = defaultRuby;
	options.logLevel = getLogLevel();
	options.loggingAgentAddress = loggingAgentAddress;
	options.loggingAgentUsername = P_STATIC_STRING("logging");
	options.loggingAgentPassword = loggingAgentPassword;
	if (!this->defaultUser.empty()) {
		options.defaultUser = defaultUser;
	}
	if (!this->defaultGroup.empty()) {
		options.defaultGroup = defaultGroup;
	}
	options.minProcesses = agentsOptions->getInt("min_instances");
	options.spawnMethod = agentsOptions->get("spawn_method");
	options.statThrottleRate = statThrottleRate;
}

static void
fillPoolOption(Request *req, StaticString &field, const HashedStaticString &name) {
	const LString *value = req->secureHeaders.lookup(name);
	if (value != NULL && value->size > 0) {
		value = psg_lstr_make_contiguous(value, req->pool);
		field = StaticString(value->start->data, value->size);
	}
}

static void
fillPoolOption(Request *req, bool &field, const HashedStaticString &name) {
	const LString *value = req->secureHeaders.lookup(name);
	if (value != NULL && value->size > 0) {
		field = psg_lstr_first_byte(value) == 't';
	}
}

static void
fillPoolOption(Request *req, unsigned int &field, const HashedStaticString &name) {
	const LString *value = req->secureHeaders.lookup(name);
	if (value != NULL && value->size > 0) {
		value = psg_lstr_make_contiguous(value, req->pool);
		field = stringToUint(StaticString(value->start->data, value->size));
	}
}

static void
fillPoolOption(Request *req, unsigned long &field, const HashedStaticString &name) {
	const LString *value = req->secureHeaders.lookup(name);
	if (value != NULL && value->size > 0) {
		value = psg_lstr_make_contiguous(value, req->pool);
		field = stringToUint(StaticString(value->start->data, value->size));
	}
}

static void
fillPoolOption(Request *req, long &field, const HashedStaticString &name) {
	const LString *value = req->secureHeaders.lookup(name);
	if (value != NULL && value->size > 0) {
		value = psg_lstr_make_contiguous(value, req->pool);
		field = stringToInt(StaticString(value->start->data, value->size));
	}
}

static void
fillPoolOptionSecToMsec(Request *req, unsigned int &field, const HashedStaticString &name) {
	const LString *value = req->secureHeaders.lookup(name);
	if (value != NULL && value->size > 0) {
		value = psg_lstr_make_contiguous(value, req->pool);
		field = stringToInt(StaticString(value->start->data, value->size)) * 1000;
	}
}

void
createNewPoolOptions(Client *client, Request *req, const LString *appGroupName) {
	ServerKit::HeaderTable &secureHeaders = req->secureHeaders;
	Options &options = req->options;

	SKC_TRACE(client, 2, "Creating new pool options: app group name=" <<
		StaticString(appGroupName->start->data, appGroupName->size));

	options = Options();

	const LString *scriptName = secureHeaders.lookup("!~SCRIPT_NAME");
	const LString *appRoot = secureHeaders.lookup("!~PASSENGER_APP_ROOT");
	if (scriptName == NULL || scriptName->size == 0) {
		if (appRoot == NULL || appRoot->size == 0) {
			const LString *documentRoot = secureHeaders.lookup("!~DOCUMENT_ROOT");
			if (OXT_UNLIKELY(documentRoot == NULL || documentRoot->size == 0)) {
				disconnectWithError(&client, "client did not send a !~PASSENGER_APP_ROOT or a !~DOCUMENT_ROOT header");
				return;
			}

			documentRoot = psg_lstr_make_contiguous(documentRoot, req->pool);
			appRoot = psg_lstr_create(req->pool,
				extractDirNameStatic(StaticString(documentRoot->start->data,
					documentRoot->size)));
		} else {
			appRoot = psg_lstr_make_contiguous(appRoot, req->pool);
		}
		options.appRoot = StaticString(appRoot->start->data, appRoot->size);
	} else {
		if (appRoot == NULL || appRoot->size == 0) {
			const LString *documentRoot = secureHeaders.lookup("!~DOCUMENT_ROOT");
			if (OXT_UNLIKELY(documentRoot == NULL || documentRoot->size == 0)) {
				disconnectWithError(&client, "client did not send a !~DOCUMENT_ROOT header");
				return;
			}

			documentRoot = psg_lstr_null_terminate(documentRoot, req->pool);
			documentRoot = resolveSymlink(StaticString(documentRoot->start->data,
				documentRoot->size), req->pool);
			appRoot = psg_lstr_create(req->pool,
				extractDirNameStatic(StaticString(documentRoot->start->data,
					documentRoot->size)));
		} else {
			appRoot = psg_lstr_make_contiguous(appRoot, req->pool);
		}
		options.appRoot = StaticString(appRoot->start->data, appRoot->size);
		scriptName = psg_lstr_make_contiguous(scriptName, req->pool);
		options.baseURI = StaticString(scriptName->start->data, scriptName->size);
	}

	fillPoolOptionsFromAgentsOptions(options);

	const LString *appType = secureHeaders.lookup("!~PASSENGER_APP_TYPE");
	if (appType == NULL || appType->size == 0) {
		AppTypeDetector detector;
		PassengerAppType type = detector.checkAppRoot(options.appRoot);
		// TODO: check for errors
		options.appType = getAppTypeName(type);
	} else {
		fillPoolOption(req, options.appType, "!~PASSENGER_APP_TYPE");
	}

	options.appGroupName = StaticString(appGroupName->start->data, appGroupName->size);

	fillPoolOption(req, options.appType, "!~PASSENGER_APP_TYPE");
	fillPoolOption(req, options.environment, "!~PASSENGER_APP_ENV");
	fillPoolOption(req, options.ruby, "!~PASSENGER_RUBY");
	fillPoolOption(req, options.python, "!~PASSENGER_PYTHON");
	fillPoolOption(req, options.nodejs, "!~PASSENGER_NODEJS");
	fillPoolOption(req, options.user, "!~PASSENGER_USER");
	fillPoolOption(req, options.group, "!~PASSENGER_GROUP");
	fillPoolOption(req, options.minProcesses, "!~PASSENGER_MIN_PROCESSES");
	fillPoolOption(req, options.maxProcesses, "!~PASSENGER_MAX_PROCESSES");
	fillPoolOption(req, options.spawnMethod, "!~PASSENGER_SPAWN_METHOD");
	fillPoolOption(req, options.startCommand, "!~PASSENGER_START_COMMAND");
	fillPoolOptionSecToMsec(req, options.startTimeout, "!~PASSENGER_START_TIMEOUT");
	fillPoolOption(req, options.maxPreloaderIdleTime, "!~PASSENGER_MAX_PRELOADER_IDLE_TIME");
	fillPoolOption(req, options.maxRequestQueueSize, "!~PASSENGER_MAX_REQUEST_QUEUE_SIZE");
	fillPoolOption(req, options.restartDir, "!~PASSENGER_RESTART_DIR");
	fillPoolOption(req, options.startupFile, "!~PASSENGER_STARTUP_FILE");
	fillPoolOption(req, options.loadShellEnvvars, "!~PASSENGER_LOAD_SHELL_ENVVARS");
	fillPoolOption(req, options.debugger, "!~PASSENGER_DEBUGGER");
	fillPoolOption(req, options.environmentVariables, "!~PASSENGER_ENV_VARS");
	fillPoolOption(req, options.raiseInternalError, "!~PASSENGER_RAISE_INTERNAL_ERROR");
	/******************/
	fillPoolOption(req, options.rollingRestart, "!~PASSENGER_ROLLING_RESTARTS");
	fillPoolOption(req, options.ignoreSpawnErrors, "!~PASSENGER_RESIST_DEPLOYMENT_ERRORS");
	fillPoolOption(req, options.memoryLimit, "!~PASSENGER_MEMORY_LIMIT");
	fillPoolOption(req, options.concurrencyModel, "!~PASSENGER_CONCURRENCY_MODEL");
	fillPoolOption(req, options.threadCount, "!~PASSENGER_THREAD_COUNT");

	boost::shared_ptr<Options> optionsCopy = boost::make_shared<Options>(options);
	optionsCopy->persist(options);
	optionsCopy->clearPerRequestFields();
	optionsCopy->detachFromUnionStationTransaction();
	poolOptionsCache.insert(options.getAppGroupName(), optionsCopy);
}

void
initializeUnionStation(Client *client, Request *req, RequestAnalysis &analysis) {
	if (analysis.unionStationSupport) {
		Options &options = req->options;
		ServerKit::HeaderTable &headers = req->secureHeaders;

		const LString *key = headers.lookup("!~UNION_STATION_KEY");
		if (key == NULL || key->size == 0) {
			disconnectWithError(&client, "header !~UNION_STATION_KEY must be set.");
			return;
		}
		key = psg_lstr_make_contiguous(key, req->pool);

		const LString *filters = headers.lookup("!~UNION_STATION_FILTERS");
		if (filters != NULL) {
			filters = psg_lstr_make_contiguous(filters, req->pool);
		}

		options.transaction = unionStationCore->newTransaction(
			options.getAppGroupName(), "requests",
			string(key->start->data, key->size),
			(filters != NULL)
				? string(filters->start->data, filters->size)
				: string());
		if (!options.transaction->isNull()) {
			options.analytics = true;
			options.unionStationKey = StaticString(key->start->data, key->size);
		}

		req->beginScopeLog(&req->scopeLogs.requestProcessing, "request processing");
		req->logMessage(string("Request method: ") + http_method_str(req->method));
		req->logMessage("URI: " + StaticString(req->path.start->data, req->path.size));
	}
}

void
setStickySessionId(Client *client, Request *req) {
	if (req->stickySession) {
		// TODO: This is not entirely correct. Clients MAY send multiple Cookie
		// headers, although this is in practice extremely rare.
		// http://stackoverflow.com/questions/16305814/are-multiple-cookie-headers-allowed-in-an-http-request
		const LString *cookieHeader = req->headers.lookup(HTTP_COOKIE);
		const LString *cookieName = getStickySessionCookieName(req);
		vector< pair<StaticString, StaticString> > cookies;
		pair<StaticString, StaticString> cookie;

		parseCookieHeader(req->pool, cookieHeader, cookies);
		foreach (cookie, cookies) {
			if (psg_lstr_cmp(cookieName, cookie.first)) {
				// This cookie matches the one we're looking for.
				req->options.stickySessionId = stringToUint(cookie.second);
				return;
			}
		}
	}
}

const LString *
getStickySessionCookieName(Request *req) {
	const LString *value = req->headers.lookup(PASSENGER_STICKY_SESSIONS_COOKIE_NAME);
	if (value == NULL || value->size == 0) {
		return psg_lstr_create(req->pool,
			P_STATIC_STRING(DEFAULT_STICKY_SESSIONS_COOKIE_NAME));
	} else {
		return value;
	}
}
