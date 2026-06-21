/* win-net17 -- C17a proof: a STOCK Windows winsock TCP client.
 *
 * Built with the in-tree clang as a real PE (clang --target=x86_64-w64-mingw32
 * ... -lws2_32). It resolves example.com via gethostbyname, opens a TCP
 * socket, sends an HTTP/1.0 GET, drains the response, and verifies the
 * "HTTP/1" status line. Exit 17 IFF the entire winsock chain
 * (WSAStartup -> gethostbyname -> socket -> connect -> send -> recv ->
 * closesocket -> WSACleanup) succeeds AND a real HTTP response came back --
 * a path only reachable if ws2_32 is genuinely mapped onto tobyOS's TCP/DNS
 * stack. Nothing here is tobyOS-specific: it is ordinary Winsock 2 code. */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[c17a] WSAStartup FAIL\n");
        return 1;
    }
    printf("[c17a] WSAStartup ok ver=%u.%u\n",
           (unsigned)LOBYTE(wsa.wVersion), (unsigned)HIBYTE(wsa.wVersion));

    struct hostent *he = gethostbyname("example.com");
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        printf("[c17a] gethostbyname FAIL err=%d\n", WSAGetLastError());
        WSACleanup();
        return 2;
    }
    unsigned char *ip = (unsigned char *)he->h_addr_list[0];
    printf("[c17a] example.com -> %u.%u.%u.%u (h_length=%d h_addrtype=%d)\n",
           ip[0], ip[1], ip[2], ip[3], he->h_length, he->h_addrtype);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        printf("[c17a] socket FAIL err=%d\n", WSAGetLastError());
        WSACleanup();
        return 3;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        printf("[c17a] connect FAIL err=%d\n", WSAGetLastError());
        closesocket(s);
        WSACleanup();
        return 4;
    }
    printf("[c17a] connected to example.com:80\n");

    const char *req =
        "GET / HTTP/1.0\r\n"
        "Host: example.com\r\n"
        "User-Agent: tobyOS-winsock/C17a\r\n"
        "Connection: close\r\n"
        "\r\n";
    int sent = send(s, req, (int)strlen(req), 0);
    printf("[c17a] sent %d bytes\n", sent);

    char buf[2048];
    int total = 0;
    for (;;) {
        int n = recv(s, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        if (total >= (int)sizeof(buf) - 1) break;
    }
    buf[total > 0 ? total : 0] = 0;
    printf("[c17a] recv total=%d bytes\n", total);

    closesocket(s);
    WSACleanup();

    if (total > 0) {
        char line[80];
        int i = 0;
        for (; i < (int)sizeof(line) - 1 && buf[i] && buf[i] != '\r' && buf[i] != '\n'; i++)
            line[i] = buf[i];
        line[i] = 0;
        printf("[c17a] status line: \"%s\"\n", line);
    }

    int ok = (total > 12 && strncmp(buf, "HTTP/1", 6) == 0);
    printf("[c17a] verdict %s\n", ok ? "PASS" : "FAIL");
    return ok ? 17 : 5;
}
