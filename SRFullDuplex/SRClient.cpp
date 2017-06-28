// SRClient.cpp : �������̨Ӧ�ó������ڵ㡣
//
#include "stdafx.h"
#include <stdlib.h>
#include <WinSock2.h>
#include <time.h>
#include <vector>

#pragma comment(lib,"ws2_32.lib")

#define SERVER_PORT 12340 //�������ݵĶ˿ں�
#define SERVER_IP "127.0.0.1" // �������� IP ��ַ

//��������������ָ��socket�͵�ַ�Ͻ���SR���ݵķ���
class Sender {
private:
	SOCKET sendSocket;//ͨ�����socket��������
	SOCKADDR_IN toAddr;//Ŀ���ַ����������Ҫͨ������һ�ε���recvFrom����ܰ�Ŀ���ַ
	int sendWinSize = 7;
	int timeout = 20;
	std::vector<int> timer;//Ϊÿһ�������ʱ
	int curSeq = 0;//��ǰ���ݰ��� seq
	int curAck = 0;//��ǰ�ȴ�ȷ�ϵ� ack
	int next = -1;//��ǰ�ȴ��ط���seq
	int totalSeq = 0;//curAck֮ǰ�յ��İ�������
	char *data;//�����͵����ݻ�����
	int seqSize = 20;

	bool seqIsAvailable() {
		int step;
		step = curSeq - curAck;
		if (step < 0)	step += seqSize;
		//���к��Ƿ��ڵ�ǰ���ʹ���֮��
		return step < sendWinSize;
	}
public:
	Sender(SOCKET socket, SOCKADDR_IN _toAddr, int _seqSize, char *_data) :
		sendSocket(socket), toAddr(_toAddr), timer(_seqSize, 0), seqSize(_seqSize), data(_data)
	{
		//��������ʹ��
		char *testData[10] = { "yeah", "nice", "to", "meet", "you",
		"I", "am", "super", "client", "!" };
		for (int i = 0; i < 100; ++i) {
			memcpy(data + i * 1024,
				testData[i % 10],
				1 + strlen(testData[i % 10]));
		}
	}
	int getSendWinSize() { return sendWinSize; }
	int getCurSeq() { return curSeq; }
	//�������� [curAck...curSeq) ��timer + 1
	void updateTimer() {
		if (curAck < curSeq) {
			for (int i = curSeq - 1; i >= curAck; --i) {
				if (++timer[i] > timeout)
					next = i;
			}
		}
		else {
			for (int i = curSeq - 1; i >= 0; --i) {
				if (++timer[i] > timeout)
					next = i;
			}
			for (int i = seqSize - 1; i >= curAck; --i) {
				if (++timer[i] > timeout)
					next = i;
			}
		}
	}
	//����ack�����ü�ʱ������������
	void ackHandler(char c) {
		unsigned char index = (unsigned char)c - 1;
		printf("Recv a ack of %d\n", index);
		timer[index] = -88888888; //��ʱ���ô�������ʾ�ѽ��ܣ�����+1��Ӱ��
		if (index == curAck) { //�����ƶ������������ѽ��յ�λ��
			for (; index < seqSize && timer[index] < 0; ++index, ++totalSeq);
			if (index == seqSize)
				for (index = 0; index < seqSize && timer[index] < 0; ++index, ++totalSeq);
			curAck = index;//�����µĴ�����ʼλ��
			printf("���ʹ��ڣ�[%d, %d]\n", curAck, (curAck + sendWinSize - 1) % seqSize);
		}
	}
	//�������ݣ���1�����ڻ���������2����Ҫ�ط���ʱ���ݰ�
	void send(char *buffer) {
		//���ȴ���ʱ���ݰ�������curSeq
		if (next == -1 && seqIsAvailable()) {
			next = curSeq++;
			if (curSeq == seqSize)	curSeq = 0;
		}
		if (next != -1) {
			//SEQ�ֶ�
			buffer[0] = char(next + 1);
			//ACK�ֶ�
			buffer[1] = (char)255;

			//������������ݰ������curAck��ƫ�ƣ���������buffer
			int offset = next - curAck;
			if (offset < 0)
				offset += seqSize;
			memcpy(buffer + 2,
				data + 1024 * (totalSeq + offset),
				1 + strlen(data + 1024 * (totalSeq + offset)));
			//�����ݻ��淢��
			printf("send a packet with a seq of %d\n", next);
			sendto(sendSocket, buffer, strlen(buffer) + 1, 0,
				(SOCKADDR*)&toAddr, sizeof(SOCKADDR));
			//���¼�ʱ
			timer[next] = 0;
			next = -1;
		}
	}
};
//��������������ָ��socket�͵�ַ�Ͻ���SR���ݵĽ���/�ظ�ACK
class Reciever {
	SOCKET toSocket;//���շ�socket
	SOCKADDR_IN fromAddr;//���ͷ���ַ
	int seqSize = 20;
	int recvWinSize = 5;
	std::vector<bool> store;//��־data��Ӧλ���Ƿ���Ч�����ڼ򻯣����ҽ���storeȫ������������ϲ�Ӧ��
	int begSeq = 0;//��ʾrecvData[0]��Ӧ�����к�
	int bufferSize = 0;//��ʾ����������С
	int packetLossRatio = 0.5, ackLossRatio = 0.5;
	int fromSendWinSize = 7;
	const static int BUFFER_LENGTH = 1026;
	std::vector<char[BUFFER_LENGTH]> recvData;//���ջ���

											  //�����ʧ
	bool lossInLossRatio(float lossRatio) {
		int r = rand() % 101;
		return r <= lossRatio * 100;
	}
	//�ж��Ƿ���Ҫ����ack��Ҫô�ڵ�ǰ�����ڣ�Ҫô�ڶԷ��Ĵ��������ѽ���
	bool mustAck(int seq) {
		if (seq >= begSeq && seq < begSeq + recvWinSize)//�ڵ�ǰ������
			return true;
		if (seq > begSeq)//���Ƿ��ͷ����ڴ����±����
			seq -= seqSize;
		if (seq + fromSendWinSize >= begSeq)//����ڷ��ͷ��Ĵ�����
			return true;
		return false;
	}
public:
	Reciever(SOCKET socket, SOCKADDR_IN _fromAddr, int _seqSize, int _recvWinSize)
		: toSocket(socket), fromAddr(_fromAddr), seqSize(_seqSize), recvWinSize(_recvWinSize),
		recvData(_recvWinSize), store(_recvWinSize, false) {}
	void setFromSendWinSize(int _fromSendWinSize) { fromSendWinSize = _fromSendWinSize; }
	int getFromSendWinSize() { return fromSendWinSize; }
	void setBegSeq(int _begSeq) { begSeq = _begSeq; }
	int getBegSeq() { return begSeq; }
	//�������ݰ�
	void recieve(char *buffer) {
		int seq = -1 + (int)buffer[0];
		//�����ģ����Ƿ�ʧ
		bool b = lossInLossRatio(packetLossRatio);
		if (b) {
			printf("The packet with a seq of %d loss\n", seq);
			return;
		}
		printf("recv a packet with a seq of %d\n", seq);
		//������ڴ��İ�����ȷ���գ�����ȷ�ϼ���
		if (begSeq <= seq && seq < begSeq + 5 && !store[seq - begSeq]) {
			store[seq - begSeq] = true;
			++bufferSize;
			memcpy(recvData[seq - begSeq], buffer + 2, 1 + strlen(buffer + 2));
			if (bufferSize == recvWinSize) {
				//��������
				bufferSize = 0;
				begSeq += recvWinSize;
				if (begSeq >= seqSize)
					begSeq -= seqSize;
				printf("���մ��ڣ� [%d, %d]\n", begSeq, (begSeq + recvWinSize - 1) % seqSize);
				for (int i = 0; i < recvWinSize; ++i)
					store[i] = false;
				//��������data�е�����
				printf("���յ����������ݣ� ");
				for (int i = 0; i < recvWinSize; ++i)
					printf("%s ", recvData[i]);
				printf("\n");
			}
		}
		//����Ѿ�ȷ�Ϲ��ð����򷵻�ack
		if (mustAck(seq)) {
			//ACK�ֶ�
			buffer[1] = (char)(seq + 1);
			buffer[2] = '\0';

			b = lossInLossRatio(ackLossRatio);
			if (b) {
				printf("The ack of %d loss\n", seq);
				return;
			}
			sendto(toSocket, buffer, 3, 0,
				(SOCKADDR*)&fromAddr, sizeof(SOCKADDR));
			printf("send a ack of %d\n", seq);
		}
	}
};

SOCKET init() {
	//�����׽��ֿ⣨���룩
	WORD wVersionRequested;
	WSADATA wsaData;
	//�׽��ּ���ʱ������ʾ
	int err;
	//�汾 2.2
	wVersionRequested = MAKEWORD(2, 2);
	//���� dll �ļ� Scoket ��
	err = WSAStartup(wVersionRequested, &wsaData);
	if (err != 0) {
		//�Ҳ��� winsock.dll
		printf("WSAStartup failed with error: %d\n", err);
		return 1;
	}
	if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)
	{
		printf("Could not find a usable version of Winsock.dll\n");
		WSACleanup();
	}
	else {
		printf("The Winsock 2.2 dll was found okay\n");
	}
	SOCKET socketClient = socket(AF_INET, SOCK_DGRAM, 0);

	SOCKADDR_IN addrClient;
	addrClient.sin_addr.S_un.S_addr = htonl(INADDR_ANY);//���߾���
	addrClient.sin_family = AF_INET;
	addrClient.sin_port = htons(SERVER_PORT + 1);
	err = bind(socketClient, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
	if (err) {
		err = GetLastError();
		printf("Could not bind the port %d for socket.Error code is %d\n", SERVER_PORT, err);
		WSACleanup();
		return -1;
	}
	return socketClient;
}

char dataReady[1024 * 100];
const int BUFFER_LENGTH = 1026;
const int SEQ_SIZE = 20;//���ն����кŸ�����Ϊ 1~20

SOCKADDR_IN getServerAddr() {
	SOCKADDR_IN addrServer;
	addrServer.sin_addr.S_un.S_addr = inet_addr(SERVER_IP);
	addrServer.sin_family = AF_INET;
	addrServer.sin_port = htons(SERVER_PORT);
	return addrServer;
}

void firstShake(char *buffer) {
	buffer[0] = (char)220;
	buffer[1] = (char)0;
	buffer[2] = (char)7;
	buffer[3] = '\0';
}

int main(int argc, char* argv[])
{
	SOCKET socketClient = init();
	SOCKADDR_IN addrServer = getServerAddr();
	char buffer[BUFFER_LENGTH];
	int len = sizeof(SOCKADDR), recvSize;

	float packetLossRatio = 0.2; //Ĭ�ϰ���ʧ�� 0.2
	float ackLossRatio = 0.2; //Ĭ�� ACK ��ʧ�� 0.2
	srand((unsigned)time(NULL));

	while (true) {//����������
		firstShake(buffer);
		sendto(socketClient, buffer, strlen(buffer) + 1, 0, (SOCKADDR*)&addrServer, len);
		recvSize = recvfrom(socketClient, buffer, BUFFER_LENGTH, 0, (SOCKADDR*)&addrServer, &len);
		if (buffer[0] == (char)250) {
			int iMode = 1; //1����������0������
			ioctlsocket(socketClient, FIONBIO, (u_long FAR*) &iMode);//����������
			break;
		}
		Sleep(1000);
	}

	Sender sender(socketClient, addrServer, SEQ_SIZE, dataReady);
	Reciever reciever(socketClient, addrServer, SEQ_SIZE, 5);
	reciever.setBegSeq(int(buffer[1]));
	reciever.setFromSendWinSize(int(buffer[2]));
	printf("��ʼ˫��������begSeq = %d, winSize = %d\n", reciever.getBegSeq(), reciever.getFromSendWinSize());

	while (true) {
		while ((recvSize =
			recvfrom(socketClient, buffer, BUFFER_LENGTH, 0,
			((SOCKADDR*)&addrServer), &len)) >= 0) {
			if (buffer[1] != (char)255) {//�յ�ack
				printf("�յ�ACK\n");
				sender.ackHandler(buffer[1]);
			}
			else {//�յ�����
				printf("�յ�����\n");
				reciever.recieve(buffer);
			}
		}
		sender.updateTimer();
		sender.send(buffer);
		Sleep(5000);
	}
	//�ر��׽���
	closesocket(socketClient);
	WSACleanup();
	return 0;
}
