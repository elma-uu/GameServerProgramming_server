#pragma once
#include "common.h"

enum IOType { IO_SEND, IO_RECV, IO_ACCEPT };

class RingBuffer {
	public:
	char buffer[MAX_BUF_SIZE];
	int head;
	int tail;
	int size;
	RingBuffer() : head(0), tail(0), size(0) {}
	bool push(const char* data, int len) {
		if (len > MAX_BUF_SIZE - size) return false; // Not enough space
		for (int i = 0; i < len; ++i) {
			buffer[tail] = data[i];
			tail = (tail + 1) % MAX_BUF_SIZE;
		}
		size += len;
		return true;
	}
	int pop(char* dest, int len) {
		if (len > size) return -1; // Not enough data
		for (int i = 0; i < len; ++i) {
			dest[i] = buffer[head];
			head = (head + 1) % MAX_BUF_SIZE;
		}
		size -= len;
		return len;
	}
	int get_size() const { return size; }
};


class EXP_OVER {
public:
	WSAOVERLAPPED m_over;
	IOType  m_iotype;
	WSABUF	m_wsa;
	SOCKET  m_client_socket;
	RingBuffer m_ring_buffer;
	EXP_OVER()
	{
		ZeroMemory(&m_over, sizeof(m_over));
		m_wsa.buf = m_ring_buffer.buffer;
		m_wsa.len = MAX_BUF_SIZE;
		m_client_socket = INVALID_SOCKET;
	}
	EXP_OVER(IOType iot) : m_iotype(iot)
	{
		ZeroMemory(&m_over, sizeof(m_over));
		m_wsa.buf = m_ring_buffer.buffer;
		m_wsa.len = MAX_BUF_SIZE;
		m_client_socket = INVALID_SOCKET;
	}
};
