// GBNServer.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h" //创建 VS 项目包含的预编译头文件
#include <stdlib.h>
#include <time.h>
#include <WinSock2.h>
#include <fstream>
#include <vector>

#pragma comment(lib,"ws2_32.lib")

#define SERVER_PORT 12340 //端口号
#define SERVER_IP "0.0.0.0" //IP 地址

class Sender {
private:
	SOCKET sendSocket;//通过这个socket发送数据
	SOCKADDR_IN toAddr;//目标地址！！！！需要通过至少一次调用recvFrom后才能绑定目标地址
	int sendWinSize = 7;
	int timeout = 20;
	std::vector<int> timer;//为每一个缓存计时
	int curSeq = 0;//当前数据包的 seq
	int curAck = 0;//当前等待确认的 ack
	int next = -1;//当前等待重发的seq
	int totalSeq = 0;//curAck之前收到的包的总数
	char *data;//待发送的数据缓冲区
	int seqSize = 20;

	bool seqIsAvailable() {
		int step;
		step = curSeq - curAck;
		if (step < 0)	step += seqSize;
		//序列号是否在当前发送窗口之内
		return step < sendWinSize;
	}
public:
	Sender(SOCKET socket, SOCKADDR_IN _toAddr, int _seqSize, char *_data) :
		sendSocket(socket), toAddr(_toAddr), timer(_seqSize, 0), seqSize(_seqSize), data(_data)
	{
		//仅供测试使用，将数据存储到data中
		char *testData[10] = { "hello", "this", "is", "sr", "test",
			"good", "luck", "to", "you", "!" };
		for (int i = 0; i < 100; ++i) {
			memcpy(data + i * 1024,
				testData[i % 10],
				1 + strlen(testData[i % 10]));
		}
	}
	int getSendWinSize() { return sendWinSize; }
	int getCurSeq() { return curSeq; }
	//更新区间 [curAck...curSeq) 的timer + 1
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
	//处理ack：设置计时器、滑动窗口
	void ackHandler(char c) {
		unsigned char index = (unsigned char)c - 1;
		printf("Recv a ack of %d\n", index);
		timer[index] = -88888888; //计时器置大负数，表示已接受，不受+1的影响
		if (index == curAck) { //窗口移动，跳过所有已接收的位置
			for (; index < seqSize && timer[index] < 0; ++index, ++totalSeq);
			if (index == seqSize)
				for (index = 0; index < seqSize && timer[index] < 0; ++index, ++totalSeq);
			curAck = index;//设置新的窗口起始位置
			printf("发送窗口：[%d, %d]\n", curAck, (curAck + sendWinSize - 1) % seqSize);
		}
	}
	//发送数据：（1）窗口还有容量（2）需要重发超时数据包
	void send(char *buffer) {
		//优先传超时数据包，否则传curSeq
		if (next == -1 && seqIsAvailable()) {
			next = curSeq++;
			if (curSeq == seqSize)	curSeq = 0;
		}
		if (next != -1) {
			//SEQ字段
			buffer[0] = char(next + 1);
			//ACK字段
			buffer[1] = (char)255;

			//计算待发送数据包的相对curAck的偏移，并拷贝到buffer
			int offset = next - curAck;
			if (offset < 0)
				offset += seqSize;
			memcpy(buffer + 2,
				data + 1024 * (totalSeq + offset),
				1 + strlen(data + 1024 * (totalSeq + offset)));
			//将数据缓存发出
			printf("send a packet with a seq of %d\n", next);
			sendto(sendSocket, buffer, strlen(buffer) + 1, 0,
				(SOCKADDR*)&toAddr, sizeof(SOCKADDR));
			//重新计时
			timer[next] = 0;
			next = -1;
		}
	}
};

class Reciever {
	SOCKET toSocket;//接收方socket
	SOCKADDR_IN fromAddr;//发送方地址
	int seqSize = 20;
	int recvWinSize = 5;
	std::vector<bool> store;//标志data相应位置是否有效，出于简化，当且仅当store全满，打包交付上层应用
	int begSeq = 0;//表示recvData[0]对应的序列号
	int bufferSize = 0;//表示缓存已填充大小
	int packetLossRatio = 0.5, ackLossRatio = 0.5;//丢包率
	int fromSendWinSize = 7;//发送方窗口大小
	const static int BUFFER_LENGTH = 1026;
	std::vector<char[BUFFER_LENGTH]> recvData;//接收缓存

	//随机丢失
	bool lossInLossRatio(float lossRatio) {
		int r = rand() % 101;
		return r <= lossRatio * 100;
	}
	//判断是否需要返回ack：要么在当前窗口内，要么在对方的窗口内且已接受
	bool mustAck(int seq) {
		if (seq >= begSeq && seq < begSeq + recvWinSize)//在当前窗口内
			return true;
		if (seq > begSeq)//考虑发送方窗口存在下标溢出
			seq -= seqSize;
		if (seq + fromSendWinSize >= begSeq)//如果在发送方的窗口里
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
	//接收数据包
	void recieve(char *buffer) {
		int seq = -1 + (int)buffer[0];
		//随机法模拟包是否丢失
		bool b = lossInLossRatio(packetLossRatio);
		if (b) {
			printf("The packet with a seq of %d loss\n", seq);
			return;
		}
		printf("recv a packet with a seq of %d\n", seq);
		//如果是期待的包，正确接收，正常确认即可
		if (begSeq <= seq && seq < begSeq + 5 && !store[seq - begSeq]) {
			store[seq - begSeq] = true;
			++bufferSize;
			memcpy(recvData[seq - begSeq], buffer + 2, 1 + strlen(buffer + 2));
			if (bufferSize == recvWinSize) {
				//滑动窗口
				bufferSize = 0;
				begSeq += recvWinSize;
				if (begSeq >= seqSize)
					begSeq -= seqSize;
				printf("接收窗口： [%d, %d]\n", begSeq, (begSeq + recvWinSize - 1) % seqSize);
				for (int i = 0; i < recvWinSize; ++i)
					store[i] = false;
				//处理整合data中的数据
				printf("接收到的整合数据： ");
				for (int i = 0; i < recvWinSize; ++i)
					printf("%s ", recvData[i]);
				printf("\n");
			}
		}
		//如果已经确认过该包，则返回ack
		if (mustAck(seq)) {
			//ACK字段
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

void getCurTime(char *ptime) {
	char buffer[128];
	memset(buffer, 0, sizeof(buffer));
	time_t c_time;
	struct tm *p;
	time(&c_time);
	p = localtime(&c_time);
	sprintf_s(buffer, "%d/%d/%d %d:%d:%d",
		p->tm_year + 1900,
		p->tm_mon,
		p->tm_mday,
		p->tm_hour,
		p->tm_min,
		p->tm_sec);
	strcpy_s(ptime, sizeof(buffer), buffer);
}
SOCKET init() {
	//加载套接字库（必须）
	WORD wVersionRequested;
	WSADATA wsaData;
	//套接字加载时错误提示
	int err;
	//版本 2.2
	wVersionRequested = MAKEWORD(2, 2);
	//加载 dll 文件 Scoket 库
	err = WSAStartup(wVersionRequested, &wsaData);
	if (err != 0) {
		//找不到 winsock.dll
		printf("WSAStartup failed with error: %d\n", err);
		return -1;
	}
	if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)
	{
		printf("Could not find a usable version of Winsock.dll\n");
		WSACleanup();
	}
	else {
		printf("The Winsock 2.2 dll was found okay\n");
	}
	SOCKET sockServer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	SOCKADDR_IN addrServer; //服务器地址
							//addrServer.sin_addr.S_un.S_addr = inet_addr(SERVER_IP);
	addrServer.sin_addr.S_un.S_addr = htonl(INADDR_ANY);//两者均可
	addrServer.sin_family = AF_INET;
	addrServer.sin_port = htons(SERVER_PORT);
	err = bind(sockServer, (SOCKADDR*)&addrServer, sizeof(SOCKADDR));
	if (err) {
		err = GetLastError();
		printf("Could not bind the port %d for socket.Error code is %d\n", SERVER_PORT, err);
		WSACleanup();
		return -1;
	}
	return sockServer;
}

char dataReady[1024 * 100]; //待发送数据
const int SEQ_SIZE = 20;
const int BUFFER_LENGTH = 1026;

void secondShake(char *buffer) {
	buffer[0] = (char)250;
	buffer[1] = (char)0;
	buffer[2] = (char)7;
	buffer[3] = '\0';
}

//主函数
int main(int argc, char* argv[])
{
	SOCKET sockServer = init();
	SOCKADDR_IN addrClient; //客户端地址
	int length = sizeof(SOCKADDR), recvSize;
	char buffer[BUFFER_LENGTH];

	while (true) {
		recvSize = recvfrom(sockServer, buffer, BUFFER_LENGTH, 0, ((SOCKADDR*)&addrClient), &length);
		if (buffer[0] == (char)220) {
			secondShake(buffer);
			sendto(sockServer, buffer, BUFFER_LENGTH, 0, ((SOCKADDR*)&addrClient), length);
			//设置套接字为非阻塞模式
			int iMode = 1; //1：非阻塞，0：阻塞
			ioctlsocket(sockServer, FIONBIO, (u_long FAR*) &iMode);//非阻塞设置
			break;
		}
	}

	Sender sender(sockServer, addrClient, SEQ_SIZE, dataReady);
	Reciever reciever(sockServer, addrClient, SEQ_SIZE, 5);
	reciever.setBegSeq(int(buffer[1]));
	reciever.setFromSendWinSize(int(buffer[2]));
	printf("开始双方交流：begSeq = %d, winSize = %d\n", reciever.getBegSeq(), reciever.getFromSendWinSize());

	while (true) {
		while ((recvSize =
			recvfrom(sockServer, buffer, BUFFER_LENGTH, 0,
			((SOCKADDR*)&addrClient), &length)) >= 0) {
			if (buffer[1] != (char)255) {//收到ack
				printf("收到ACK\n");
				sender.ackHandler(buffer[1]);
			}
			else {//收到数据
				printf("收到数据\n");
				reciever.recieve(buffer);
			}
		}
		sender.updateTimer();
		sender.send(buffer);
		Sleep(5000);
	}
	//关闭套接字，卸载库
	closesocket(sockServer);
	WSACleanup();
	return 0;
}