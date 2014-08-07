#include "mage.h"

using namespace jsonrpc;

namespace mage {

    static std::vector<std::__thread_id> s_cancelThread;

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
		this->Cancel();

		delete m_pJsonRpcClient;
		delete m_pHttpClient;
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

		/**
		 * Todo?:
		 *   foreach Event
		 *     call event callback
		 */
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

    void RPC::Call(const std::string& name,
                   const Json::Value& params,
                   const std::function<void(mage::MageError, Json::Value)>& callback) {
		m_task = std::thread([this, name, params, callback] {
			Json::Value res;
			mage::MageSuccess ok;
            
			try {
				res = Call(name, params);
				if (!IsCancelAndCleanThread(std::this_thread::get_id())) callback(ok, res);
			} catch (mage::MageError e) {
				if (!IsCancelAndCleanThread(std::this_thread::get_id())) callback(e, res);
			}
		});
    }

	void RPC::SetDomain(const std::string& mageDomain) {
		m_sDomain = mageDomain;
		m_pHttpClient->SetUrl(GetUrl());
	}

	void RPC::SetApplication(const std::string& mageApplication) {
		m_sApplication = mageApplication;
		m_pHttpClient->SetUrl(GetUrl());
	}

	void RPC::SetProtocol(const std::string& mageProtocol) {
		m_sProtocol = mageProtocol;
		m_pHttpClient->SetUrl(GetUrl());
	}

	void RPC::SetSession(const std::string& sessionKey) const {
		m_pHttpClient->AddHeader("X-MAGE-SESSION", sessionKey);
	}

	void RPC::ClearSession() const {
		m_pHttpClient->RemoveHeader("X-MAGE-SESSION");
	}

	std::string RPC::GetUrl() const {
		return m_sProtocol + "://" + m_sDomain + "/" + m_sApplication + "/jsonrpc";
	}

    void RPC::Cancel() {
        if (m_task.joinable()) {
            s_cancelThread.push_back(m_task.get_id());
            m_task.detach();
        }
    }
    
    bool RPC::IsCancelAndCleanThread(std::__thread_id threadId) {
        if (find(s_cancelThread.begin(), s_cancelThread.end() , threadId) != s_cancelThread.end()) {
            s_cancelThread.erase(std::remove(s_cancelThread.begin(), s_cancelThread.end(), threadId), s_cancelThread.end());
            return true;
        }
        
        return false;
    }
}  // namespace mage
