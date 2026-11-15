#include "MailBox.h"

bool CMailBox::IsPending() const
{
	return m_calls.size() != 0;
}

void CMailBox::WaitForCall()
{
	std::unique_lock callLock(m_callMutex);
	while(!IsPending())
	{
		m_waiters++;
		m_waitCondition.wait(callLock);
		m_waiters--;
		m_wakeSent = false;
	}
}

void CMailBox::WaitForCall(unsigned int timeOut)
{
	std::unique_lock callLock(m_callMutex);
	if(IsPending()) return;
	m_waiters++;
	m_waitCondition.wait_for(callLock, std::chrono::milliseconds(timeOut));
	m_waiters--;
	m_wakeSent = false;
}

void CMailBox::FlushCalls()
{
	SendCall([]() {}, true);
}

void CMailBox::SendCall(const FunctionType& function, bool waitForCompletion)
{
	std::future<void> future;
	bool wake = false;

	{
		MESSAGE message;
		message.function = function;

		if(waitForCompletion)
		{
			message.promise = std::make_unique<std::promise<void>>();
			future = message.promise->get_future();
		}

		std::lock_guard callLock(m_callMutex);
		m_calls.push_back(std::move(message));
		wake = (m_waiters != 0) && !m_wakeSent;
		m_wakeSent |= wake;
	}

	if(wake)
	{
		m_waitCondition.notify_all();
	}

	if(waitForCompletion)
	{
		future.wait();
	}
}

void CMailBox::SendCall(FunctionType&& function)
{
	bool wake = false;
	{
		MESSAGE message;
		message.function = std::move(function);

		std::lock_guard callLock(m_callMutex);
		m_calls.push_back(std::move(message));
		wake = (m_waiters != 0) && !m_wakeSent;
		m_wakeSent |= wake;
	}

	if(wake)
	{
		m_waitCondition.notify_all();
	}
}

void CMailBox::ReceiveCall()
{
	MESSAGE message;
	{
		std::lock_guard callLock(m_callMutex);
		if(!IsPending()) return;
		message = std::move(m_calls.front());
		m_calls.pop_front();
	}
	message.function();
	if(message.promise)
	{
		message.promise->set_value();
	}
}
