/* win-srv17 -- C17b proof: a STOCK Windows winsock TCP SERVER.
 *
 * Built with the in-tree clang as a real PE (-lws2_32). It binds port 8080,
 * listens, accepts ONE inbound connection, reads the request, looks for the
 * magic token "TOBYPING", replies with a small HTTP response, and exits 17
 * IFF the magic token arrived -- a path only reachable if a REAL external
 * client connected and its bytes were delivered through tobyOS's TCP stack.
 *
 * The external client is the QEMU host: the boot harness runs this server,
 * QEMU forwards host 127.0.0.1:18080 -> guest 10.0.2.15:8080 (SLIRP hostfwd),
 * and the host-side script connects once the "[c17b] listening" marker shows.
 * Nothing here is tobyOS-specific: ordinary Winsock 2 server code. */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("[c17b] WSAStartup FAIL\n"); return 1; }

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { printf("[c17b] socket FAIL err=%d\n", WSAGetLastError()); return 2; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(8080);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        printf("[c17b] bind FAIL err=%d\n", WSAGetLastError());
        return 3;
    }
    if (listen(ls, 1) != 0) {
        printf("[c17b] listen FAIL err=%d\n", WSAGetLastError());
        return 4;
    }
    printf("[c17b] listening on :8080\n");   /* host script waits for this marker */

    struct sockaddr_in ca;
    int calen = (int)sizeof(ca);
    SOCKET cs = accept(ls, (struct sockaddr *)&ca, &calen);
    if (cs == INVALID_SOCKET) {
        printf("[c17b] accept FAIL err=%d\n", WSAGetLastError());
        closesocket(ls);
        return 5;
    }
    printf("[c17b] accepted a client\n");

    char buf[512];
    int n = recv(cs, buf, (int)sizeof(buf) - 1, 0);
    if (n <= 0) {
        printf("[c17b] recv FAIL n=%d err=%d\n", n, WSAGetLastError());
        closesocket(cs); closesocket(ls);
        return 6;
    }
    buf[n] = 0;
    int gotmagic = (strstr(buf, "TOBYPING") != NULL);
    printf("[c17b] recv %d bytes magic=%d\n", n, gotmagic);

    const char *resp =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 9\r\n"
        "\r\n"
        "TOBYPONG\n";
    int sent = send(cs, resp, (int)strlen(resp), 0);
    printf("[c17b] sent %d bytes response\n", sent);

    closesocket(cs);
    closesocket(ls);
    WSACleanup();

    printf("[c17b] verdict %s\n", gotmagic ? "PASS" : "FAIL");
    return gotmagic ? 17 : 7;
}
