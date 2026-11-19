#include "MailBox.h"

CMailBox::CMailBox()
{
	m_calls.reserve(1024);
	m_dispatchCalls.reserve(1024);
}

bool CMailBox::IsPending() const
{
	return m_readIndex < m_calls.size();
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

void CMailBox::PushMessage(MESSAGE&& message)
{
	bool wake = false;
	{
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

void CMailBox::SendCall(FunctionType&& function)
{
	MESSAGE message;
	message.function = std::move(function);
	PushMessage(std::move(message));
}

void CMailBox::DispatchMessage(MESSAGE& message)
{
	if(message.smallInvoke)
	{
		message.smallInvoke(message.smallStorage);
	}
	else
	{
		message.function();
	}
	if(message.promise)
	{
		message.promise->set_value();
	}
}

void CMailBox::ReceiveCall()
{
	MESSAGE message;
	{
		std::lock_guard callLock(m_callMutex);
		if(m_readIndex >= m_calls.size()) return;
		message = std::move(m_calls[m_readIndex++]);
		if(m_readIndex >= m_calls.size())
		{
			m_calls.clear();
			m_readIndex = 0;
		}
	}
	DispatchMessage(message);
}

void CMailBox::ReceiveAllCalls()
{
	while(true)
	{
		size_t startIndex = 0;
		{
			std::lock_guard callLock(m_callMutex);
			if(m_readIndex >= m_calls.size())
			{
				m_calls.clear();
				m_readIndex = 0;
				return;
			}
			m_dispatchCalls.clear();
			m_dispatchCalls.swap(m_calls);
			startIndex = m_readIndex;
			m_readIndex = 0;
		}
		for(size_t i = startIndex; i < m_dispatchCalls.size(); i++)
		{
			DispatchMessage(m_dispatchCalls[i]);
		}
		m_dispatchCalls.clear();
	}
}
