#pragma once

#include <functional>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <future>

class CMailBox
{
public:
	virtual ~CMailBox() = default;

	typedef std::function<void()> FunctionType;

	void SendCall(const FunctionType&, bool = false);
	void SendCall(FunctionType&&);
	void FlushCalls();

	bool IsPending() const;
	void ReceiveCall();
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

		FunctionType function;
		std::unique_ptr<std::promise<void>> promise;
	};

	typedef std::deque<MESSAGE> FunctionCallQueue;

	FunctionCallQueue m_calls;
	std::mutex m_callMutex;
	std::condition_variable m_waitCondition;
	//Receivers waiting on m_waitCondition (guarded by m_callMutex): senders only wake it when there are some
	unsigned int m_waiters = 0;
	//A wake was sent and no receiver has woken up since: more calls don't need another one
	bool m_wakeSent = false;
};
