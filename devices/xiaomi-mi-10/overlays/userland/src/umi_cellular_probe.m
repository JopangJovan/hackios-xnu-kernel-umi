/* One-shot diagnostic for an explicit SDX55 WDS IPv4 data call. No default
 * resolver or default-route sockets are used in the external probe. */
#import <Foundation/Foundation.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
/* IFM_ACTIVE in XNU net/if_media.h, absent from the iOS SDK. */
#define IFM_ACTIVE 0x00000002
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include "umi_cell_probe_wire.h"

extern char **environ;
typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef, CFStringRef, void *, void *);
extern CFArrayRef SCDynamicStoreCopyKeyList(SCDynamicStoreRef, CFStringRef);
extern CFPropertyListRef SCDynamicStoreCopyValue(SCDynamicStoreRef, CFStringRef);

static bool link_active(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return false;
	struct ifmediareq media = {0};
	strlcpy(media.ifm_name, "en1", sizeof(media.ifm_name));
	bool active = ioctl(fd, SIOCGIFMEDIA, &media) == 0 && (media.ifm_status & IFM_ACTIVE);
	close(fd);
	return active;
}

static bool address_ready(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return false;
	struct ifreq req = {0};
	strlcpy(req.ifr_name, "en1", sizeof(req.ifr_name));
	bool ok = ioctl(fd, SIOCGIFADDR, &req) == 0;
	uint32_t addr = ntohl(((struct sockaddr_in *)&req.ifr_addr)->sin_addr.s_addr);
	close(fd);
	return ok && addr && (addr >> 16) != 0xa9fe;
}

/* Only consume DNS from a service whose IPv4 state explicitly names en1. */
static unsigned resolver_list(SCDynamicStoreRef store, struct in_addr *servers, unsigned capacity)
{
	NSArray *keys = CFBridgingRelease(SCDynamicStoreCopyKeyList(store, CFSTR("State:/Network/Service/[^/]+/IPv4")));
	for (NSString *key in keys) {
		NSDictionary *v4 = CFBridgingRelease(SCDynamicStoreCopyValue(store, (__bridge CFStringRef)key));
		if (![v4 isKindOfClass:NSDictionary.class] || ![v4[@"InterfaceName"] isEqual:@"en1"]) continue;
		NSString *dnsKey = [[key substringToIndex:key.length-4] stringByAppendingString:@"DNS"];
		NSDictionary *dns = CFBridgingRelease(SCDynamicStoreCopyValue(store, (__bridge CFStringRef)dnsKey));
		if (![dns isKindOfClass:NSDictionary.class] || ![dns[@"ServerAddresses"] isKindOfClass:NSArray.class]) continue;
		unsigned count = 0;
		for (id value in dns[@"ServerAddresses"]) {
			if (![value isKindOfClass:NSString.class] || inet_pton(AF_INET, [value UTF8String], &servers[count]) != 1 ||
			    !servers[count].s_addr) continue;
			if (++count == capacity) break;
		}
		if (count) return count;
	}
	return 0;
}

static void ask_dhcp(void)
{
	pid_t child;
	char *args[] = {"/redwood/bin/net_test", "dhcp", "en1", NULL};
	posix_spawn_file_actions_t files;
	if (posix_spawn_file_actions_init(&files) != 0) return;
	int error = posix_spawn_file_actions_addopen(&files,STDOUT_FILENO,"/dev/null",O_WRONLY,0);
	if (!error) error = posix_spawn_file_actions_adddup2(&files, STDOUT_FILENO, STDERR_FILENO);
	if (!error) error = posix_spawn(&child, args[0], &files, NULL, args, environ);
	posix_spawn_file_actions_destroy(&files);
	if (error) return;
	for (unsigned i=0; i<25; i++) {
		pid_t done = waitpid(child, NULL, WNOHANG);
		if (done == child || (done < 0 && errno != EINTR)) return;
		usleep(100000);
	}
	kill(child, SIGKILL);
	while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static int bound_socket(unsigned index, int type)
{
	int fd = socket(AF_INET, type, 0);
	if (fd < 0) return -1;
	struct timeval timeout = {3,0};
	if (setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index)) ||
	    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
	    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool resolve_on_en1(unsigned index, struct in_addr server, struct in_addr *answer)
{
	int fd = bound_socket(index, SOCK_DGRAM);
	if (fd < 0) return false;
	struct sockaddr_in addr = {.sin_len=sizeof(addr), .sin_family=AF_INET, .sin_port=htons(53), .sin_addr=server};
	uint8_t query[] = {0,0,1,0,0,1,0,0,0,0,0,0,7,'c','a','p','t','i','v','e',5,'a','p','p','l','e',3,'c','o','m',0,0,1,0,1};
	uint16_t xid = (uint16_t)arc4random();
	query[0] = (uint8_t)(xid >> 8);
	query[1] = (uint8_t)xid;
	uint8_t reply[4096];
	ssize_t n = -1;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 && send(fd,query,sizeof(query),0) == sizeof(query))
		n = recv(fd,reply,sizeof(reply),0);
	close(fd);
	return n > 0 && cell_probe_dns(reply,(size_t)n,xid,answer);
}

static bool http_on_en1(unsigned index, struct in_addr address)
{
	int fd = bound_socket(index, SOCK_STREAM);
	if (fd < 0) return false;
	struct sockaddr_in addr = {.sin_len=sizeof(addr), .sin_family=AF_INET, .sin_port=htons(80), .sin_addr=address};
	bool success = false;
	if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) goto done;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		struct pollfd pfd = {.fd=fd,.events=POLLOUT};
		int error;
		socklen_t len = sizeof(error);
		if (errno != EINPROGRESS || poll(&pfd,1,4000) <= 0 ||
		    getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len) != 0 || error) goto done;
	}
	if (fcntl(fd, F_SETFL, 0) < 0) goto done;
	const char req[] = "GET /hotspot-detect.html HTTP/1.0\r\nHost: " CELL_PROBE_HOST "\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
	size_t offset = 0;
	while (offset < sizeof(req)-1) {
		ssize_t n = send(fd,req+offset,sizeof(req)-1-offset,0);
		if (n <= 0) goto done;
		offset += n;
	}
	char response[8192];
	size_t used = 0;
	while (used < sizeof(response)-1) {
		ssize_t n = recv(fd,response+used,sizeof(response)-1-used,0);
		if (n < 0) goto done;
		if (!n) {
			response[used] = 0;
			success = !memchr(response,0,used) && cell_probe_http(response);
			break;
		}
		used += n;
	}
done:
	close(fd);
	return success;
}

int main(void)
{
	@autoreleasepool {
		char args[2048] = {0};
		size_t size = sizeof(args);
		if (sysctlbyname("kern.bootargs",args,&size,NULL,0) || !size || !memchr(args,0,size) || !cell_probe_enabled(args)) return 0;
		setvbuf(stdout,NULL,_IONBF,0);
		signal(SIGPIPE,SIG_IGN);
		alarm(180);
		puts("UMI cellular probe: waiting for en1");
		for (unsigned i=0; i<120 && !link_active(); i++) sleep(1);
		unsigned index = if_nametoindex("en1");
		if (!index || !link_active()) { puts("UMI cellular probe: no active en1"); return 1; }
		for (unsigned i=0; i<3 && !address_ready(); i++) sleep(1);
		SCDynamicStoreRef store = NULL;
		struct in_addr servers[2], answer;
		unsigned count = 0;
		for (unsigned i = 0; i < 18 && link_active(); i++) {
			@autoreleasepool {
				if (!store) store = SCDynamicStoreCreate(NULL,CFSTR("UMI cellular probe"),NULL,NULL);
				if (address_ready()) {
					if (store) count = resolver_list(store,servers,2);
				} else if (i % 6 == 0) {
					/* IPConfiguration may still be loading at the first link event. */
					ask_dhcp();
				}
			}
			if (count) break;
			sleep(1);
		}
		if (!store) { puts("UMI cellular probe: configd unavailable"); return 1; }
		CFRelease(store);
		if (!count) { puts("UMI cellular probe: no en1 IPv4 lease with service DNS"); return 1; }
		bool resolved=false;
		for (unsigned i=0; i<count && !resolved; i++) resolved = resolve_on_en1(index,servers[i],&answer);
		if (!resolved) { puts("UMI cellular probe: en1 DNS check failed"); return 1; }
		puts("UMI cellular probe: en1-bound DNS answer verified");
		if (!http_on_en1(index,answer)) { puts("UMI cellular probe: en1-bound HTTP check failed"); return 1; }
		puts("UMI cellular probe: en1-bound HTTP 200 with Apple connectivity response");
		return 0;
	}
}
