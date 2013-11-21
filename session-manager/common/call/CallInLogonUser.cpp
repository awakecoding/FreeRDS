/**
 * Class for rpc call LogonUser (freerds to session manager)
 *
 * Copyright 2013 Thinstuff Technologies GmbH
 * Copyright 2013 DI (FH) Martin Haimberger <martin.haimberger@thinstuff.at>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "CallInLogonUser.h"
#include <appcontext/ApplicationContext.h>
#include <module/AuthModule.h>

using freerds::icp::LogonUserRequest;
using freerds::icp::LogonUserResponse;

namespace freerds
{
	namespace sessionmanager
	{
		namespace call
		{

		static wLog* logger_CallInLogonUser = WLog_Get("freerds.SessionManager.call.callinlogonuser");


		CallInLogonUser::CallInLogonUser()
			: mConnectionId(0), mAuthStatus(0)
		{

		};

		CallInLogonUser::~CallInLogonUser()
		{

		};

		unsigned long CallInLogonUser::getCallType()
		{
			return freerds::icp::LogonUser;
		};

		int CallInLogonUser::decodeRequest()
		{
			// decode protocol buffers
			LogonUserRequest req;

			if (!req.ParseFromString(mEncodedRequest))
			{
				// failed to parse
				mResult = 1;// will report error with answer
				return -1;
			}

			mUserName = req.username();

			mConnectionId = req.connectionid();

			mDomainName = req.domain();

			mPassword = req.password();

			return 0;
		};

		int CallInLogonUser::encodeResponse()
		{
			// encode protocol buffers
			LogonUserResponse resp;
			// stup do stuff here

			resp.set_authstatus(mAuthStatus);
			resp.set_serviceendpoint(mPipeName);

			if (!resp.SerializeToString(&mEncodedResponse))
			{
				// failed to serialize
				mResult = 1;
				return -1;
			}

			return 0;
		};

		int CallInLogonUser::authenticateUser() {

			std::string authModule;
			if (!APP_CONTEXT.getPropertyManager()->getPropertyString(0,"auth.module",authModule,mUserName)) {
				authModule = "PAM";
			}

			moduleNS::AuthModule* auth = moduleNS::AuthModule::loadFromName(authModule);

			if (!auth) {
				mResult = 1;
				return 1;
			}

			mAuthStatus = auth->logonUser(mUserName, mDomainName, mPassword);

			delete auth;
			return 0;

		}

		int CallInLogonUser::getUserSession() {

			sessionNS::Session* currentSession;
			sessionNS::Connection * currentConnection = APP_CONTEXT.getConnectionStore()->getOrCreateConnection(mConnectionId);

			if (currentConnection->getSessionId() != 0) {
				currentSession = APP_CONTEXT.getSessionStore()->getSession(currentConnection->getSessionId());
				if (currentSession == NULL) {
					mResult = 1;
					return -1;
				}
				// we had an auth session before ...
				if (currentSession->isAuthSession()) {
					currentSession->stopModule();
					APP_CONTEXT.getSessionStore()->removeSession(currentSession->getSessionID());
					currentConnection->setSessionId(0);
				} else {
					WLog_Print(logger_CallInLogonUser, WLOG_ERROR, "Expected session to be an authsession with sessionId = %d",currentSession->getSessionID());
				}
			} else {
				// check if there is an running session, which is disconnected
				currentSession = APP_CONTEXT.getSessionStore()->getFirstSessionUserName(mUserName, mDomainName);
			}


			if ((!currentSession) || (currentSession->getConnectState() != WTSDisconnected))
			{
				// create new Session for this request
				currentSession = APP_CONTEXT.getSessionStore()->createSession();
				currentSession->setUserName(mUserName);
				currentSession->setDomain(mDomainName);

				if (!currentSession->generateUserToken())
				{
					WLog_Print(logger_CallInLogonUser, WLOG_ERROR, "generateUserToken failed for user %s with domain %s",mUserName.c_str(),mDomainName.c_str());
					mResult = 1;// will report error with answer
					return 1;
				}

				if (!currentSession->generateEnvBlockAndModify())
				{
					WLog_Print(logger_CallInLogonUser, WLOG_ERROR, "generateEnvBlockAndModify failed for user %s with domain %s",mUserName.c_str(),mDomainName.c_str());
					mResult = 1;// will report error with answer
					return 1;
				}
				std::string moduleName;

				if (!APP_CONTEXT.getPropertyManager()->getPropertyString(currentSession->getSessionID(),"module",moduleName)) {
					moduleName = "X11";
				}
				currentSession->setModuleName(moduleName);
			}

			if (currentSession->getConnectState() == WTSDown)
			{
				std::string pipeName;
				if (!currentSession->startModule(pipeName))
				{
					WLog_Print(logger_CallInLogonUser, WLOG_ERROR, "Module %s does not start properly for user %s in domain %s",currentSession->getModuleName().c_str(),mUserName.c_str(),mDomainName.c_str());
					mResult = 1;// will report error with answer
					return 1;
				}
			}

			currentConnection->setSessionId(currentSession->getSessionID());
			mPipeName = currentSession->getPipeName();
			return 0;
		}

		int CallInLogonUser::getAuthSession() {
			// authentication failed, start up greeter module
			sessionNS::Session* currentSession;
			sessionNS::Connection * currentConnection = APP_CONTEXT.getConnectionStore()->getOrCreateConnection(mConnectionId);

			if (currentConnection->getSessionId() != 0 ){
				// we had a session for authentication before ... use this session
				currentSession = APP_CONTEXT.getSessionStore()->getSession(currentConnection->getSessionId());
				if (currentSession == NULL) {
					mResult = 1;
					return -1;
				}
			} else {
				currentSession = APP_CONTEXT.getSessionStore()->createSession();
				std::string greeter;

				if (!APP_CONTEXT.getPropertyManager()->getPropertyString(0,"auth.greeter",greeter,mUserName)) {
					greeter = "Qt";
				}
				currentSession->setModuleName(greeter);
				if (!currentSession->startModule(greeter))
				{
					mResult = 1;// will report error with answer
					return 1;
				}
				currentConnection->setSessionId(currentSession->getSessionID());
			}
			currentSession->setAuthSession(true);
			mPipeName = currentSession->getPipeName();
			return 0;
		}

		int CallInLogonUser::doStuff()
		{

			if (authenticateUser() != 0) {
				return 1;
			}
			if (mAuthStatus != 0) {
				if (getAuthSession() != 0 ) {
					return 1;
				}
			} else {
				// user is authenticated
				if (getUserSession() != 0) {
					return 1;
				}
			}
			return 0;
		}

		}
	}
}
