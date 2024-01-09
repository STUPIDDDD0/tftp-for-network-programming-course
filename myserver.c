#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#define DEFAULT_PORT 8888 // TFTP的默认端口
#define MAX_BUFFER 65536 // 最大缓冲区大小，足够容纳最大的TFTP块大小
#define DEFAULT_BLOCK_SIZE 512 // TFTP的默认块大小

// TFTP操作码
enum {
    OP_RRQ = 1, // 读请求
    OP_WRQ = 2, // 写请求
    OP_DATA = 3, // 数据
    OP_ACK = 4, // 确认
    OP_ERROR = 5 // 错误
};

void handle_rrq(int sock, struct sockaddr_in *client_addr, socklen_t client_len, char *filename, int block_size);
void handle_wrq(int sock, struct sockaddr_in *client_addr, socklen_t client_len, char *filename, int block_size);
void send_error(int sock, struct sockaddr_in *client_addr, socklen_t client_len, int error_code, char *error_msg);

int main() {
    int sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[MAX_BUFFER];
    int received_bytes;

    // 创建UDP套接字
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("创建套接字失败");
        exit(EXIT_FAILURE);
    }

    // 绑定套接字到TFTP端口
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(DEFAULT_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("绑定套接字失败");
        close(sock);
        exit(EXIT_FAILURE);
    }

    printf("TFTP服务器启动，等待连接...\n");

    // 循环以接收请求
    while (1) {
        received_bytes = recvfrom(sock, buffer, MAX_BUFFER, 0, (struct sockaddr *)&client_addr, &client_len);
        if (received_bytes < 0) {
            perror("接收数据失败");
            continue;
        }

        // 检查操作码
        if (received_bytes < 2) {
            send_error(sock, &client_addr, client_len, 0, "请求太短");
            continue;
        }

        int op_code = ntohs(*(short *)buffer);

        // 分析文件名和模式，提取块大小（如果有）
        char *filename = buffer + 2;
        char *mode = filename;
        while (*mode && mode < buffer + received_bytes) mode++;
        if (mode == buffer + received_bytes || mode == filename) {
            send_error(sock, &client_addr, client_len, 0, "无法解析文件名和模式");
            continue;
        }
        mode++; // 跳过结束的0
        char *options = mode;
        while (*options && options < buffer + received_bytes) options++;
        options++; // 跳过结束的0

        int block_size = DEFAULT_BLOCK_SIZE;
        while (options < buffer + received_bytes) {
            if (strcasecmp(options, "blksize") == 0) {
                options += strlen(options) + 1;
                block_size = atoi(options);
                if (block_size < 1 || block_size > MAX_BUFFER) {
                    send_error(sock, &client_addr, client_len, 0, "无效的块大小");
                    block_size = DEFAULT_BLOCK_SIZE;
                }
                break;
            }
            options += strlen(options) + 1;
        }


        // 根据操作码处理RRQ或WRQ
        switch (op_code) {
            case OP_RRQ:
                printf("收到读请求，文件名: %s，模式: %s\n", filename, mode);
                handle_rrq(sock, &client_addr, client_len, filename, block_size);
                break;
            case OP_WRQ:
                printf("收到写请求，文件名: %s，模式: %s\n", filename, mode);
                handle_wrq(sock, &client_addr, client_len, filename, block_size);
                break;
            default:
                send_error(sock, &client_addr, client_len, 0, "不支持的操作码");
                break;
        }
    }

    // 关闭套接字
    close(sock);
    return 0;
}

void handle_rrq(int sock, struct sockaddr_in *client_addr, socklen_t client_len, char *filename, int block_size) {
    FILE *file;
    char buffer[MAX_BUFFER];
    int block_num = 1;
    int read_bytes;
    int send_result;

    // 打开文件以供读取
    file = fopen(filename, "r");
    if (file == NULL) {
        send_error(sock, client_addr, client_len, 1, "文件未找到");
        return;
    }

    // 循环发送文件内容
    do {
        buffer[0] = 0;
        buffer[1] = OP_DATA;
        *(short *)(buffer + 2) = htons(block_num);

        // 读取文件块
        read_bytes = fread(buffer + 4, 1, block_size, file);
        if (read_bytes < 0) {
            send_error(sock, client_addr, client_len, 0, "文件读取错误");
            break;
        }

        // 发送数据包
        send_result = sendto(sock, buffer, read_bytes + 4, 0, (struct sockaddr *)client_addr, client_len);
        if (send_result < 0) {
            perror("发送数据包失败");
            break;
        }

        // 等待ACK
        struct sockaddr_in ack_addr;
        socklen_t ack_len = sizeof(ack_addr);
        int recv_result = recvfrom(sock, buffer, MAX_BUFFER, 0, (struct sockaddr *)&ack_addr, &ack_len);
        if (recv_result < 0) {
            perror("接收ACK失败");
            break;
        }

        // 验证ACK
        if (buffer[1] != OP_ACK || ntohs(*(short *)(buffer + 2)) != block_num) {
            send_error(sock, client_addr, client_len, 0, "ACK验证失败");
            break;
        }

        block_num++;
    } while (read_bytes == block_size); // 如果读取的字节小于block_size，那么这是最后一个数据块

    fclose(file);
}

void handle_wrq(int sock, struct sockaddr_in *client_addr, socklen_t client_len, char *filename, int block_size) {
    FILE *file;
    char buffer[MAX_BUFFER];
    int recv_len, write_len;
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    unsigned short block_num = 0;
    int timeout_count = 0;

    // 打开文件准备写入
    file = fopen(filename, "wb");
    if (file == NULL) {
        send_error(sock, client_addr, client_len, 0, "无法打开文件");
        return;
    }

    // 发送ACK 0以确认WRQ
    *(short *)buffer = htons(OP_ACK);
    *(short *)(buffer + 2) = htons(block_num);
    sendto(sock, buffer, 4, 0, (struct sockaddr *)client_addr, client_len);

    // 接收数据并写入文件
    while (1) {
        // 接收数据包
        recv_len = recvfrom(sock, buffer, MAX_BUFFER, 0, (struct sockaddr *)&from_addr, &from_len);
        if (recv_len < 0) {
            perror("接收数据失败");
            break;
        }

        // 验证是否是数据包并且块编号正确
        if (ntohs(*(short *)buffer) == OP_DATA && ntohs(*(short *)(buffer + 2)) == (block_num + 1)) {
            block_num++;
            timeout_count = 0; // 重置超时计数器
            printf("ACK %d\n",block_num);

            // 写入数据块到文件
            write_len = fwrite(buffer + 4, 1, recv_len - 4, file);
            if (write_len < recv_len - 4) {
                send_error(sock, &from_addr, from_len, 0, "文件写入错误");
                break;
            }

            // 发送ACK
            *(short *)buffer = htons(OP_ACK);
            *(short *)(buffer + 2) = htons(block_num);
            sendto(sock, buffer, 4, 0, (struct sockaddr *)&from_addr, from_len);

            // 最后的数据包，退出循环
            if (recv_len < block_size + 4) {
                break;
            }
        } else if (ntohs(*(short *)buffer) == OP_ERROR) {
            // 如果收到错误包，打印错误并退出
            printf("收到错误包: %s\n", buffer + 4);
            break;
        } else {
            // 如果收到的不是预期的数据包，发送错误消息
            send_error(sock, &from_addr, from_len, 0, "未知错误");
            break;
        }
    }

    // 关闭文件
    fclose(file);
}

void send_error(int sock, struct sockaddr_in *client_addr, socklen_t client_len, int error_code, char *error_msg) {
    char buffer[MAX_BUFFER];
    int error_msg_len = strlen(error_msg);

    // 构造错误包
    *(short *)buffer = htons(OP_ERROR);
    *(short *)(buffer + 2) = htons(error_code);
    strcpy(buffer + 4, error_msg);

    // 发送错误包
    sendto(sock, buffer, 4 + error_msg_len + 1, 0, (struct sockaddr *)client_addr, client_len);
}