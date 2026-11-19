#pragma once

#include <cstring>
#include <functional>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <future>
#include <type_traits>

class CMailBox
{
public:
	CMailBox();
	virtual ~CMailBox() = default;

	typedef std::function<void()> FunctionType;

	void SendCall(const FunctionType&, bool = false);
	void SendCall(FunctionType&&);

	//Zero-allocation fast path for small trivially-copyable callables (e.g. GS write/image batches).
	template <typename F, typename std::enable_if<
	                          !std::is_same<typename std::decay<F>::type, FunctionType>::value &&
	                          std::is_trivially_copyable<typename std::decay<F>::type>::value &&
	                          (sizeof(typename std::decay<F>::type) <= 48) &&
	                          (alignof(typename std::decay<F>::type) <= 8),
	                          int>::type = 0>
	void SendCall(F&& fn)
	{
		using DecayF = typename std::decay<F>::type;
		MESSAGE message;
		message.smallInvoke = [](void* p) { (*reinterpret_cast<DecayF*>(p))(); };
		DecayF copy(std::forward<F>(fn));
		std::memcpy(message.smallStorage, &copy, sizeof(DecayF));
		PushMessage(std::move(message));
	}

	void FlushCalls();

	bool IsPending() const;
	void ReceiveCall();
	void ReceiveAllCalls();
	void WaitForCall();
	void WaitForCall(unsigned int);

private:
	struct MESSAGE
	{
		MESSAGE() = default;

		MESSAGE(MESSAGE&&) = default;
		MESSAGE(const MESSAGE&) = delete;

		MESSAGE& operator=(MESSAGE&&) = default;
		MESSAGE& operator=(const MESSAGE&) = delete;

		void (*smallInvoke)(void*) = nullptr;
		alignas(8) unsigned char smallStorage[48];
		FunctionType function;
		std::unique_ptr<std::promise<void>> promise;
	};

	void PushMessage(MESSAGE&&);
	static void DispatchMessage(MESSAGE&);

	std::vector<MESSAGE> m_calls;
	std::vector<MESSAGE> m_dispatchCalls;
	size_t m_readIndex = 0;
	std::mutex m_callMutex;
	std::condition_variable m_waitCondition;
	//Receivers waiting on m_waitCondition (guarded by m_callMutex): senders only wake it when there are some
	unsigned int m_waiters = 0;
	//A wake was sent and no receiver has woken up since: more calls don't need another one
	bool m_wakeSent = false;
};
