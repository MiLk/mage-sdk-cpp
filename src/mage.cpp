#include "mage.h"

#include <curl/curl.h>

#include <chrono>
#include <thread>

#ifndef SHORTPOLLING_INTERVAL
	#define SHORTPOLLING_INTERVAL 5
#endif

using namespace jsonrpc;

namespace mage {

	RPC::RPC(const std::string& mageApplication,
	         const std::string& mageDomain,
	         const std::string& mageProtocol)
	: m_sProtocol(mageProtocol)
	, m_sDomain(mageDomain)
	, m_sApplication(mageApplication) {
		m_pHttpClient    = new HttpClient(GetUrl());
		m_pJsonRpcClient = new Client(m_pHttpClient);
	}

	RPC::~RPC() {
		delete m_pJsonRpcClient;
		delete m_pHttpClient;
	}

	void RPC::ExtractEventsFromCommandResponse(const Json::Value& myEvents) const {
		for (unsigned int i = 0; i < myEvents.size(); ++i) {
			// We can only handle string
			if (!myEvents[i].isString()) {
				continue;
			}

			Json::Reader reader;
			Json::Value event;
			if (!reader.parse(myEvents[i].asString(), event)) {
				std::cerr << "Unable to read the following event: "
						  << myEvents[i] << std::endl;
				continue;
			}

			switch (event.size()) {
				case 1:
					ReceiveEvent(event[0u].asString());
					break;
				case 2:
					ReceiveEvent(event[0u].asString(), event[1u]);
					break;
				default:
					std::cerr << "The event doesn't have the correct "
							  << "amount of data."
							  << std::endl
							  << event.toStyledString()
							  << std::endl;
			}
		}
	}

	Json::Value RPC::Call(const std::string& name,
	                      const Json::Value& params) const {
		Json::Value res;

		try {
			m_pJsonRpcClient->CallMethod(name, params, res);
		} catch (JsonRpcException ex) {
			throw MageRPCError(ex.GetCode(), ex.GetMessage());
		}

		if (res.isMember("errorCode")) {
			throw MageErrorMessage(res["errorCode"].asString());
		}

		// If the myEvents array is present
		if (res.isMember("myEvents") && res["myEvents"].isArray()) {
			ExtractEventsFromCommandResponse(res["myEvents"]);
		}

		return res;
	}

	std::future<Json::Value> RPC::Call(const std::string& name,
	                                   const Json::Value& params,
	                                   bool doAsync) const {
		std::launch policy = doAsync ? std::launch::async : std::launch::deferred;

		return std::async(policy, [this, name, params]{
			return Call(name, params);
		});
	}

	std::future<void> RPC::Call(const std::string& name,
	               const Json::Value& params,
	               const std::function<void(mage::MageError, Json::Value)>& callback,
	               bool doAsync) const {
		std::launch policy = doAsync ? std::launch::async : std::launch::deferred;

		return std::async(policy, [this, name, params, callback]{
			Json::Value res;
			mage::MageSuccess ok;

			try {
				res = Call(name, params);
				callback(ok, res);
			} catch (mage::MageError e) {
				callback(e, res);
			}
		});
	}

	void RPC::ReceiveEvent(const std::string& name, const Json::Value& data) const {
		std::list<EventObserver*>::const_iterator citr;
		for(citr = m_oObjserverList.cbegin();
		    citr != m_oObjserverList.cend(); ++citr) {
			(*citr)->ReceiveEvent(name, data);
		}
	}

	void RPC::AddObserver(EventObserver* observer) {
		m_oObjserverList.push_back(observer);
	}

	static int writer(char *data, size_t size, size_t nmemb,
	                  std::string *writerData) {
		if (writerData == NULL) return 0;
		writerData->append(data, size*nmemb);
		return size * nmemb;
	}

	void RPC::DoHttpGet(std::string *buffer, const std::string& url) const {
		CURL* c = curl_easy_init();
		if (!c) {
			// TODO throw
			std::cerr << "Unable to pull events. "
			          << "Unable to initialize curl."
			          << std::endl;
			return;
		}

		curl_easy_setopt(c, CURLOPT_URL, url.c_str());
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writer);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, buffer);

		CURLcode res = curl_easy_perform(c);

		curl_easy_cleanup(c);

		if(res != CURLE_OK) {
			std::cerr << "Unable to pull events. Curl error: "
			          << curl_easy_strerror(res) << std::endl;
			// TODO throw
		}
	}

	void RPC::ExtractEventsFromMsgStreamResponse(const std::string& response) {
		Json::Reader reader;
		Json::Value messages;
		if (!reader.parse(response.c_str(), messages)) {
			std::cerr << "Unable to parse the received content from "
			          << "the message stream." << std::endl;
			// TODO throw
			return;
		}

		std::vector<std::string> members = messages.getMemberNames();
		std::vector<std::string>::const_iterator citr;
		for (citr = members.begin();
		    citr != members.end();
		    ++citr) {
			for (int i = 0; i < messages[(*citr)].size(); ++i) {
				Json::Value event = messages[(*citr)][i];
				switch (event.size()) {
					case 1:
						ReceiveEvent(event[0u].asString());
						break;
					case 2:
						ReceiveEvent(event[0u].asString(), event[1u]);
						break;
					default:
						// Don't throw here to not loose any valid event
						std::cerr << "This event doesn't have the correct "
						          << "amount of data."
						          << std::endl
						          << event.toStyledString()
						          << std::endl;
						break;
				}
			}
			m_oMsgToConfirm.push_back(*citr);
		}
	}

	void RPC::PullEvents(Transport transport) {
		const std::string url = GetMsgStreamUrl(transport);
		std::string buffer;

		DoHttpGet(&buffer, url);

		// The previous messages were confirmed
		m_oMsgToConfirm.clear();

		// No messages to read
		if (buffer.empty()) {
			return;
		}

		// Heartbeat sent by MAGE
		if (buffer == "HB") {
			return;
		}

		ExtractEventsFromMsgStreamResponse(buffer);
	}

	void RPC::StartPolling(Transport transport) {
		if (m_sSessionKey.empty()) {
			std::cerr << "No session key registered." << std::endl;
			// TODO throw
			return;
		}

		auto f = [this, transport](){
			for (;;) {
				PullEvents(transport);

				// In case of shortpolling we have to wait
				if (transport == SHORTPOLLING) {
					std::this_thread::sleep_for(std::chrono::seconds(SHORTPOLLING_INTERVAL));
				}
			}
		};

		// Execute the lambda in a new thread
		std::thread t(f);

		// Separates the thread of execution from the thread object,
		// allowing execution to continue independently.
		// Any allocated resources will be freed once the thread exits.
		t.detach();
	}

	void RPC::SetProtocol(const std::string& mageProtocol) {
		m_sProtocol = mageProtocol;
		m_pHttpClient->SetUrl(GetUrl());
	}

	void RPC::SetDomain(const std::string& mageDomain) {
		m_sDomain = mageDomain;
		m_pHttpClient->SetUrl(GetUrl());
	}

	void RPC::SetApplication(const std::string& mageApplication) {
		m_sApplication = mageApplication;
		m_pHttpClient->SetUrl(GetUrl());
	}

	void RPC::SetSession(const std::string& sessionKey) {
		m_pHttpClient->AddHeader("X-MAGE-SESSION", sessionKey);
		m_sSessionKey = sessionKey;
	}

	void RPC::ClearSession() const {
		m_pHttpClient->RemoveHeader("X-MAGE-SESSION");
	}

	std::string RPC::GetUrl() const {
		return m_sProtocol + "://" + m_sDomain + "/" + m_sApplication + "/jsonrpc";
	}

	std::string RPC::GetConfirmIds() const {
		std::stringstream ss;

		bool first = true;

		std::list<std::string>::const_iterator citr;
		for (citr = m_oMsgToConfirm.cbegin();
			 citr != m_oMsgToConfirm.cend();
			 ++citr) {
			if (!first) {
				ss << ",";
			} else {
				first = false;
			}
			ss << (*citr);
		}

		return ss.str();
	}

	std::string RPC::GetMsgStreamUrl(Transport transport) const {
		std::stringstream ss;
		ss << m_sProtocol
		   << "://"
		   << m_sDomain
		   << "/msgstream?transport=";

		switch (transport) {
			case SHORTPOLLING:
				ss << "shortpolling";
				break;
			case LONGPOLLING:
				ss << "longpolling";
				break;
			default:
				// TODO throw
				std::cerr << "Unsupported transport." << std::endl;
				return "";
				break;
		}

		if (m_sSessionKey.empty()) {
			// TODO throw
		}

		ss << "&sessionKey="
		   << m_sSessionKey;

		if (!m_oMsgToConfirm.empty()) {
			ss << "&confirmIds="
			   << GetConfirmIds();
		}

		return ss.str();
	}
}  // namespace mage
