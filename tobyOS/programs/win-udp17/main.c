/* win-udp17 -- C17c proof: a STOCK Windows winsock UDP echo server.
 *
 * Built with the in-tree clang as a real PE (-lws2_32). It binds UDP port 9090,
 * blocks in recvfrom for one datagram, checks for the magic token "TOBYUDP",
 * echoes the datagram back to the sender, and exits 17 IFF the token arrived
 * AND the echo send succeeded -- a path only reachable if winsock sendto/
 * recvfrom are genuinely mapped onto tobyOS's UDP stack.
 *
 * The sender is the QEMU host: boot with -netdev user,hostfwd=udp::18081-:9090,
 * and logs/c17c.sh sends a datagram to 127.0.0.1:18081 once the "[c17c] udp
 * listening" marker appears. Ordinary Winsock 2 datagram code. */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("[c17c] WSAStartup FAIL\n"); return 1; }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { printf("[c17c] socket FAIL err=%d\n", WSAGetLastError()); return 2; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(9090);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        printf("[c17c] bind FAIL err=%d\n", WSAGetLastError());
        closesocket(s);
        return 3;
    }
    printf("[c17c] udp listening on :9090\n");   /* host script waits for this marker */

    char buf[256];
    struct sockaddr_in from;
    int fromlen = (int)sizeof(from);
    int n = recvfrom(s, buf, (int)sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fromlen);
    if (n <= 0) {
        printf("[c17c] recvfrom FAIL n=%d err=%d\n", n, WSAGetLastError());
        closesocket(s);
        return 4;
    }
    buf[n] = 0;
    int gotmagic = (strstr(buf, "TOBYUDP") != NULL);
    unsigned char *fip = (unsigned char *)&from.sin_addr;
    printf("[c17c] recvfrom %d bytes magic=%d from %u.%u.%u.%u:%u\n",
           n, gotmagic, fip[0], fip[1], fip[2], fip[3], ntohs(from.sin_port));

    int sent = sendto(s, buf, n, 0, (struct sockaddr *)&from, fromlen);   /* echo back */
    printf("[c17c] echoed %d bytes\n", sent);

    closesocket(s);
    WSACleanup();

    int ok = (gotmagic && sent > 0);
    printf("[c17c] verdict %s\n", ok ? "PASS" : "FAIL");
    return ok ? 17 : 5;
}
