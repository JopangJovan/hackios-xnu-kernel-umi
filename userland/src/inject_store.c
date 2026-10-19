/*
 * libredwood-store.dylib: code of ours inside appstored (see inject.c for how it gets there).
 *
 * What it is for. The App Store opens and shows LOADING for ever. The front page is a "bag" --
 * AppleMediaServices' configuration -- and AMS will not load one until it knows the storefront, the
 * number that says which country's store this device belongs to. Everything outside the process has
 * been measured and is well:
 *   - the bag itself is fetched from this phone by hand, 56 KB in half a second, and the server
 *     hands over a storefront in the bargain: "x-set-apple-store-front: 143469-2,8" from both
 *     bag.itunes.apple.com and init.itunes.apple.com (rc.jobs.store, src/net_test.m);
 *   - appstored opens no socket at all while the store spins, so nothing is failing on the network;
 *   - the storefront cannot be put on the device from another process: AMS's "local" account is not
 *     a row in accountsd at all -- a different identifier every boot -- so there is nothing to write
 *     to and nothing that would keep (src/store_test.m).
 *
 * So the storefront is not written anywhere here either. The account is made to ANSWER with one:
 * -[ACAccount ams_storefront] is the door every consumer in AMS goes through, and its neighbours are
 * taken in the same pass because different callers knock at different ones. No persistence, no
 * accountsd, nothing that outlives the process.
 *
 * And the question that is worth more than the storefront: is a bag request ever built? From outside
 * only the socket log could answer, and only negatively, over a console that drops lines. From in
 * here it can be answered positively. -[AMSBagNetworkTask _performFetchWithAttemptedCount:account:
 * storefront:] and its _queryItemsForClientInfo:... are where a request is assembled: they are not
 * replaced, only watched, and what they were called with is written down. If neither is entered,
 * nothing about the network matters and the gate is above them.
 *
 * The storefront is 143469-2,8 because that is what Apple's own bag server returned for THIS device
 * when asked with none -- a region, picked from the address the request came from. No Apple ID is
 * signed in, none can be, and the store must still refuse to install: there is nothing to buy with.
 *
 * AppleMediaServices and Accounts are not loaded yet when this library is: the methods are taken
 * when their images arrive (_dyld_register_func_for_add_image).
 *
 * WHAT IT ANSWERED. The storefront was the gate, and it is not the only one.
 *   entered    3 -[ACAccount ams_storefront]                     (answered 143469-2,8; was (nil))
 *   entered    3 -[AMSBagActiveAccountProvider _bagAccountForAccountMediaType:mustContainStorefront:]
 *                 mustContainStorefront:0 -> the local account, mustContainStorefront:1 -> (nil)
 *   entered    1 -[AMSBagNetworkTask _performFetchWithAttemptedCount:account:storefront:]
 *                 "BAG FETCH attempt 0, storefront 143469-2,8, account local (...)" -> a promise
 *   entered    1 -[AMSBagNetworkTask _createRequestWithQueryItems:] -> <AMSMutablePromise: ...>
 *   entered    0 -[AMSBagNetworkTask URLSessionPromise]
 *   entered    0 -[AMSURLSession dataTaskPromiseWithRequest:]
 *   entered    0 -[AMSURLSession dataTaskWithRequest:]
 *   entered    0 -[NSURLSessionTask resume]
 * So with a storefront to answer with, AMS DOES build a bag request -- that question is settled, and
 * positively, from inside. And the request never becomes a transfer: it is assembled into an
 * AMSMutablePromise that nothing ever resolves, so no session is asked for, no task is made and
 * nothing is ever resumed. Not one task is resumed in the whole daemon in a six-minute session,
 * which is why the socket log never had appstored in it. The store still shows LOADING, as it must:
 * the bag never loads.
 * AND THEN, with entry and exit counted separately -- a method that hangs and a method that is
 * never called leave the same silence below them, so every wrapper counts both:
 *   entered    1 returned    1 -[AMSBagNetworkTask _createRequestWithQueryItems:]
 *   entered    0 returned    0  the four steps that finish a request (cookies, storefront header,
 *                               anisette headers, -[AMSURLSession _prepareRequest:properties:error:])
 *   entered   12 returned   12 -[AMSMutablePromise continueWithBlock:]
 *   entered    5 returned    5 -[AMSMutablePromise thenWithBlock:]
 *   entered    1 returned    1 -[AMSMutablePromise _finishWithResult:error:logDuplicateFinishes:]
 * Nothing hangs. _createRequestWithQueryItems: returns at once, with a promise -- and that promise is
 * never finished. Seventeen continuations are attached to promises in that session and exactly ONE
 * promise is ever finished, and by address it is not the request's:
 *   PROMISE 0x101709240 waited on (continue)
 *   STEP _createRequestWithQueryItems: -> <AMSMutablePromise: 0x10170b930>
 *   PROMISE 0x10170b930 waited on (then) / (continue)
 *   PROMISE 0x101414370 finished          <- the only one, and a different promise
 * So the bag request is never produced at all: the step that would produce it is waiting on
 * something that never arrives, which is why no header is ever added, no session asked for, no task
 * made and nothing resumed. The anisette guess is ruled out with the other three: none is entered.
 *
 * Where to look next: name the code that waits. The fault reporter here already walks frame pointers
 * (say_the_fault), and the same walk at thenWithBlock: for the request's promise would print unslid
 * return addresses that "ipsw dyld a2s" turns into the function that built the chain. The input
 * promise is in the log too, created a moment before the request's (0x101709240 above), and it is
 * never finished either.
 *
 * Two things this library learned about itself, for whoever writes the next one:
 *   - class_getInstanceMethod walks up the chain, so replacing what a class only INHERITS replaces it
 *     for everything else that inherits it too. Doing that to an NSObject category restarted
 *     appstored 31 times in one run. image_added now refuses anything the superclass answers to.
 *   - an ObjC exception ends in abort(), not in SIGSEGV, so a fault reporter that watches only the
 *     memory signals says nothing at all about the usual way a replacement of ours kills a daemon.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */
#define STOREFRONT "143469-2,8"                 /* what bag.itunes.apple.com returned for this device */

static uintptr_t slide;
static CFStringRef storefront;

/* the table itself is at the end, where its functions are; the fetch hook reports what is missing */
struct replacement {
	const char *class_name;
	const char *selector;
	bool class_method;
	IMP replacement;
	IMP *original;
	bool done;
};
static struct replacement replacements[];
static size_t replacement_count(void);

/*
 * How many times each hook was entered: a count that survives a console that drops lines. The names
 * below are in the order the hooks count themselves in, which is NOT the order of the table at the
 * end -- the first version of this printed one against the other and every count came out against
 * the wrong name, which had to be undone by hand afterwards. Keep the two lists apart on purpose.
 */
enum {
	COUNT_ams_storefront, COUNT_cachedStorefront, COUNT_bagStorefront_fixed, COUNT_bagStorefront_active,
	COUNT_copyStorefront, COUNT_bagAccount, COUNT_performFetch, COUNT_queryItems, COUNT_performRequest,
	COUNT_performRequestBlock, COUNT_resume, COUNT_resumeLocal, COUNT_createRequest, COUNT_sessionPromise,
	COUNT_dataTaskPromise, COUNT_dataTask, COUNT_addCookies, COUNT_addStorefrontHeader, COUNT_addAnisette,
	COUNT_prepareRequest, COUNT_finishWithResult, COUNT_finishWithError, COUNT_finishFunnel,
	COUNT_finishResultError, COUNT_thenWithBlock, COUNT_continueWithBlock, COUNT_last
};
static const char *const counted[COUNT_last] = {
	"-[ACAccount ams_storefront]", "-[AMSBagNetworkDataSource cachedStorefront]",
	"-[AMSBagFixedAccountProvider bagStorefrontForAccountMediaType:]",
	"-[AMSBagActiveAccountProvider bagStorefrontForAccountMediaType:]",
	"-[ACAccount ams_copyStorefrontFromAccount:]",
	"-[AMSBagActiveAccountProvider _bagAccountForAccountMediaType:mustContainStorefront:]",
	"-[AMSBagNetworkTask _performFetchWithAttemptedCount:account:storefront:]",
	"-[AMSBagNetworkTask _queryItemsForClientInfo:...]", "-[AMSURLSession performRequest:completion:]",
	"-[AMSURLSession performRequest:completionBlock:]", "-[NSURLSessionTask resume]",
	"-[__NSCFLocalSessionTask resume]", "-[AMSBagNetworkTask _createRequestWithQueryItems:]",
	"-[AMSBagNetworkTask URLSessionPromise]", "-[AMSURLSession dataTaskPromiseWithRequest:]",
	"-[AMSURLSession dataTaskWithRequest:]", "ams_addCookiesForAccount:clientInfo:bag:",
	"ams_addStoreFrontHeaderForAccount:client:withBag:", "ams_addAnisetteHeadersForAccount:type:bag:",
	"-[AMSURLSession _prepareRequest:properties:error:]", "-[AMSMutablePromise finishWithResult:]",
	"-[AMSMutablePromise finishWithError:]", "-[AMSMutablePromise _finishWithResult:error:logDuplicateFinishes:]",
	"-[AMSMutablePromise finishWithResult:error:]", "-[AMSMutablePromise thenWithBlock:]",
	"-[AMSMutablePromise continueWithBlock:]",
};
static unsigned long entered[COUNT_last];
/*
 * And how many times each came BACK. A method that hangs and a method that is never called look the
 * same in one counter -- both leave everything below them at zero -- so every wrapper counts its
 * entry and its return separately. The one with entered > returned is where the thread is.
 */
static unsigned long returned[COUNT_last];
static unsigned serial;


/* Each line of ours alone on the console: it drops them under the start's flood (see rc.jobs.store). */
static void
say(const char *format, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

/* an object as a short string, for the log: a description, cut to fit */
static const char *
text_of(id object, char *buffer, size_t size)
{
	CFStringRef description;

	buffer[0] = 0;
	if (object == NULL) {
		return "(nil)";
	}
	description = (CFStringRef)((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description"));
	if (description == NULL || !CFStringGetCString(description, buffer, (CFIndex)size, kCFStringEncodingUTF8)) {
		return "(?)";
	}
	return buffer;
}

static unsigned
step_in(int which)
{
	unsigned n = __sync_add_and_fetch(&serial, 1);

	entered[which]++;
	say("redwood: appstored: -> #%u %s\n", n, counted[which]);
	return n;
}

static void
step_out(int which, unsigned n, bool got_something)
{
	returned[which]++;
	say("redwood: appstored: <- #%u %s%s\n", n, counted[which], got_something ? "" : " (nil)");
}

/*
 * The fault reporter, as in inject.c: iOS answers a crash in a stock daemon with
 * "Failed to send exception EXC_CORPSE_NOTIFY" and nothing else, so a fault in here would be a
 * silent exit. This turns one into an address, unslid, which "ipsw dyld a2s" turns into a name.
 */
static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, lr = 0, fp = 0;
	char frames[8 * 20 + 1] = "";
	size_t used = 0;

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
		fp = (uintptr_t)context->uc_mcontext->__ss.__fp;
	}
	for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;      /* the chain only ever goes up the stack */
	}
	say("redwood: appstored: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void
watch_for_faults(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	/* an ObjC exception ends in abort(), and a bad replacement of ours is likelier to do that than
	   to touch bad memory: without these a daemon that dies of us leaves nothing at all */
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
}

/* ---- the storefront, answered ----------------------------------------------------------------- */

static id (*old_ams_storefront)(id, SEL);
static id (*old_cached_storefront)(id, SEL);
static id (*old_bag_storefront)(id, SEL, id);
static id (*old_bag_storefront_active)(id, SEL, id);
static id (*old_copy_storefront)(id, SEL, id);
static id (*old_bag_account)(id, SEL, id, bool);
static id (*old_perform_fetch)(id, SEL, long, id, id);
static id (*old_query_items)(id, SEL, id, id, id, id, id, id);
static id (*old_perform_request)(id, SEL, id, id);
static id (*old_perform_request_block)(id, SEL, id, id);
static void (*old_resume_base)(id, SEL);
static void (*old_resume_local)(id, SEL);
static id (*old_create_request)(id, SEL, id);
static id (*old_session_promise)(id, SEL);
static id (*old_data_task_promise)(id, SEL, id);
static id (*old_data_task)(id, SEL, id);

static id
new_ams_storefront(id self, SEL _cmd)
{
	entered[COUNT_ams_storefront]++;
	id was = old_ams_storefront != NULL ? old_ams_storefront(self, _cmd) : NULL;
	static bool said;

	if (!said) {
		char buffer[128];

		said = true;
		say("redwood: appstored: ams_storefront was %s, now " STOREFRONT "\n", text_of(was, buffer, sizeof(buffer)));
	}
	returned[COUNT_ams_storefront]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_cached_storefront(id self, SEL _cmd)
{
	entered[COUNT_cachedStorefront]++;
	id was = old_cached_storefront != NULL ? old_cached_storefront(self, _cmd) : NULL;

	say("redwood: appstored: cachedStorefront was %s\n", was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_cachedStorefront]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_bag_storefront(id self, SEL _cmd, id media_type)
{
	entered[COUNT_bagStorefront_fixed]++;
	id was = old_bag_storefront != NULL ? old_bag_storefront(self, _cmd, media_type) : NULL;
	char buffer[128];

	say("redwood: appstored: bagStorefrontForAccountMediaType:%s was %s\n", text_of(media_type, buffer, sizeof(buffer)),
	    was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_bagStorefront_fixed]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_bag_storefront_active(id self, SEL _cmd, id media_type)
{
	entered[COUNT_bagStorefront_active]++;
	id was = old_bag_storefront_active != NULL ? old_bag_storefront_active(self, _cmd, media_type) : NULL;
	char buffer[128];

	say("redwood: appstored: active bagStorefrontForAccountMediaType:%s was %s\n",
	    text_of(media_type, buffer, sizeof(buffer)), was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_bagStorefront_active]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_copy_storefront(id self, SEL _cmd, id account)
{
	entered[COUNT_copyStorefront]++;
	id was = old_copy_storefront != NULL ? old_copy_storefront(self, _cmd, account) : NULL;

	say("redwood: appstored: ams_copyStorefrontFromAccount: was %s\n",
	    was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_copyStorefront]++;
	return was != NULL ? was : (id)storefront;
}

/* not replaced, watched: this is the fallback line, and which way it goes is the question */
static id
new_bag_account(id self, SEL _cmd, id media_type, bool must_contain_storefront)
{
	entered[COUNT_bagAccount]++;
	id account = old_bag_account(self, _cmd, media_type, must_contain_storefront);
	returned[COUNT_bagAccount]++;
	char one[128], two[192];

	say("redwood: appstored: _bagAccountForAccountMediaType:%s mustContainStorefront:%d -> %s\n",
	    text_of(media_type, one, sizeof(one)), must_contain_storefront ? 1 : 0, text_of(account, two, sizeof(two)));
	return account;
}

/* ---- is a request ever built? ------------------------------------------------------------------ */

static id
new_perform_fetch(id self, SEL _cmd, long attempt, id account, id front)
{
	entered[COUNT_performFetch]++;
	char one[160], two[64];

	id promise;

	say("redwood: appstored: BAG FETCH attempt %ld, storefront %s, account %s\n", attempt,
	    text_of(front, two, sizeof(two)), text_of(account, one, sizeof(one)));
	for (size_t i = 0; i < replacement_count(); i++) {
		if (!replacements[i].done) {
			say("redwood: appstored: NOT TAKEN %c[%s %s]\n", replacements[i].class_method ? '+' : '-',
			    replacements[i].class_name, replacements[i].selector);
		}
	}
	promise = old_perform_fetch(self, _cmd, attempt, account, front);
	returned[COUNT_performFetch]++;
	say("redwood: appstored: BAG FETCH returned %s\n", promise != NULL ? "a promise" : "(nil)");
	return promise;
}

static id
new_query_items(id self, SEL _cmd, id client, id profile, id version, id cookies, id account, id front)
{
	entered[COUNT_queryItems]++;
	id items = old_query_items(self, _cmd, client, profile, version, cookies, account, front);
	char one[64], two[160];

	say("redwood: appstored: BAG QUERY profile %s, storefront %s -> %s\n", text_of(profile, one, sizeof(one)),
	    text_of(front, two, sizeof(two)), items != NULL ? "items" : "(nil)");
	return items;
}

/* the session layer: a request that is built but never sent stops between these and the wire */
static id
new_perform_request(id self, SEL _cmd, id request, id completion)
{
	entered[COUNT_performRequest]++;
	char buffer[320];

	say("redwood: appstored: AMSURLSession performRequest:completion: %s\n", text_of(request, buffer, sizeof(buffer)));
	return old_perform_request(self, _cmd, request, completion);
}

static id
new_perform_request_block(id self, SEL _cmd, id request, id completion)
{
	entered[COUNT_performRequestBlock]++;
	char buffer[320];

	say("redwood: appstored: AMSURLSession performRequest:completionBlock: %s\n", text_of(request, buffer, sizeof(buffer)));
	return old_perform_request_block(self, _cmd, request, completion);
}

/*
 * And the narrowest gate of all: every transfer, foreground or background, in process or handed to
 * nsurlsessiond, becomes a task that somebody resumes. If the bag request is built and no task is
 * ever resumed, it never became a transfer at all; if one is, the loss is below CFNetwork.
 */
static void
say_resume(id task, const char *where)
{
	char buffer[320];
	id request = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("originalRequest"));

	say("redwood: appstored: TASK RESUME (%s) %s\n", where, text_of(request, buffer, sizeof(buffer)));
}

static void
new_resume_base(id self, SEL _cmd)
{
	entered[COUNT_resume]++;
	say_resume(self, "NSURLSessionTask");
	old_resume_base(self, _cmd);
	returned[COUNT_resume]++;
}

static void
new_resume_local(id self, SEL _cmd)
{
	entered[COUNT_resumeLocal]++;
	say_resume(self, "__NSCFLocalSessionTask");
	old_resume_local(self, _cmd);
}

/*
 * What a class really answers to. The bag request is built and no task is ever resumed, so the stop
 * is between the two, in AMS's own session layer -- whose selectors are not the ones guessed from
 * the cache's strings (AMSURLSession has no performRequest:completion:). Rather than guess again,
 * ask the runtime, late enough that the console is no longer dropping lines.
 */
static void
dump_selectors(const char *class_name, const char *needle)
{
	Class cls = objc_getClass(class_name);
	unsigned count = 0;
	Method *methods = cls != NULL ? class_copyMethodList(cls, &count) : NULL;

	if (methods == NULL) {
		say("redwood: appstored: %s: no such class\n", class_name);
		return;
	}
	for (unsigned i = 0; i < count; i++) {
		const char *name = sel_getName(method_getName(methods[i]));

		if (needle == NULL || strstr(name, needle) != NULL) {
			say("redwood: appstored: %s has -%s\n", class_name, name);
		}
	}
	free(methods);
}

static void *
late_report(__unused void *ignored)
{
	sleep(150);                     /* past the start's flood, where the console keeps what it is given */
	for (size_t i = 0; i < replacement_count(); i++) {
		if (!replacements[i].done) {
			say("redwood: appstored: NOT TAKEN %c[%s %s]\n", replacements[i].class_method ? '+' : '-',
			    replacements[i].class_name, replacements[i].selector);
		}
	}
	for (int i = 0; i < COUNT_last; i++) {
		say("redwood: appstored: entered %4lu returned %4lu %s%s\n", entered[i], returned[i], counted[i],
		    entered[i] > returned[i] ? "   <-- STILL IN IT" : "");
	}
	dump_selectors("AMSURLSession", "equest");
	dump_selectors("AMSMutablePromise", NULL);
	dump_selectors("AMSBagNetworkTask", " queryItems");
	dump_selectors("AMSBagNetworkTask", NULL);
	return NULL;
}

/*
 * The four steps between a bag request and a transfer, as the runtime says AMS really spells them:
 * AMSBagNetworkTask builds the request (_createRequestWithQueryItems:), asks for its session
 * (URLSessionPromise -- made lazily, and AMS has a line about being deallocated "while lazily
 * instantiating its AMSURLSession"), and the session makes a task (dataTaskPromiseWithRequest: or
 * dataTaskWithRequest:) which somebody resumes. resume is never reached, so the stop is one of
 * these; each says what it was given and what it gave back.
 */
static id
new_create_request(id self, SEL _cmd, id items)
{
	id request;
	char buffer[320];

	entered[COUNT_createRequest]++;
	request = old_create_request(self, _cmd, items);
	returned[COUNT_createRequest]++;
	say("redwood: appstored: STEP _createRequestWithQueryItems: -> %s\n", text_of(request, buffer, sizeof(buffer)));
	return request;
}

static id
new_session_promise(id self, SEL _cmd)
{
	id promise;
	char buffer[200];

	entered[COUNT_sessionPromise]++;
	promise = old_session_promise(self, _cmd);
	returned[COUNT_sessionPromise]++;
	say("redwood: appstored: STEP URLSessionPromise -> %s\n", text_of(promise, buffer, sizeof(buffer)));
	return promise;
}

static id
new_data_task_promise(id self, SEL _cmd, id request)
{
	id promise;
	char buffer[200];

	entered[COUNT_dataTaskPromise]++;
	promise = old_data_task_promise(self, _cmd, request);
	returned[COUNT_dataTaskPromise]++;
	say("redwood: appstored: STEP dataTaskPromiseWithRequest: -> %s\n", text_of(promise, buffer, sizeof(buffer)));
	return promise;
}

static id
new_data_task(id self, SEL _cmd, id request)
{
	id task;
	char buffer[200];

	entered[COUNT_dataTask]++;
	task = old_data_task(self, _cmd, request);
	returned[COUNT_dataTask]++;
	say("redwood: appstored: STEP dataTaskWithRequest: -> %s\n", text_of(task, buffer, sizeof(buffer)));
	return task;
}

/*
 * What the request promise waits on. _createRequestWithQueryItems: hands back an AMSMutablePromise
 * and nothing below it is ever reached, so the thread is sitting in one of the steps that finish the
 * request: the cookies (which come from amsaccountsd), the storefront header, the anisette headers
 * (which come from AuthKit through akd, and this device has never been provisioned), or the
 * session's own preparation. Each is entered and left under its own number; the one that is entered
 * and not left is the answer. Arguments are forwarded untouched and never looked at: an anisette
 * "type" is a scalar and _prepareRequest: takes an NSError **, and a description of either would be
 * a crash, not a measurement.
 */
#define WRAP3(name) \
static void *(*old_##name)(id, SEL, void *, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b, void *c) \
{ \
	unsigned n = step_in(COUNT_##name); \
	void *result = old_##name(self, cmd, a, b, c); \
	step_out(COUNT_##name, n, result != NULL); \
	return result; \
}

#define WRAP1(name) \
static void *(*old_##name)(id, SEL, void *); \
static void * \
new_##name(id self, SEL cmd, void *a) \
{ \
	unsigned n = step_in(COUNT_##name); \
	void *result = old_##name(self, cmd, a); \
	say("redwood: appstored: #%u is for promise %p\n", n, (void *)self); \
	step_out(COUNT_##name, n, result != NULL); \
	return result; \
}

/* counters only, for the ones that would flood the console: the number is the measurement */
#define WRAPQ1(name) \
static void *(*old_##name)(id, SEL, void *); \
static void * \
new_##name(id self, SEL cmd, void *a) \
{ \
	entered[COUNT_##name]++; \
	void *result = old_##name(self, cmd, a); \
	returned[COUNT_##name]++; \
	return result; \
}
#define WRAPQ2(name) \
static void *(*old_##name)(id, SEL, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b) \
{ \
	entered[COUNT_##name]++; \
	void *result = old_##name(self, cmd, a, b); \
	returned[COUNT_##name]++; \
	return result; \
}
#define WRAPQ3(name) \
static void *(*old_##name)(id, SEL, void *, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b, void *c) \
{ \
	entered[COUNT_##name]++; \
	void *result = old_##name(self, cmd, a, b, c); \
	returned[COUNT_##name]++; \
	return result; \
}

/* the funnel every finished promise goes through, and the two ways of waiting on one */
/* this one says which promise, because one promise in seventeen is finished and we want its name */
static void *(*old_finishFunnel)(id, SEL, void *, void *, void *);
static void *
new_finishFunnel(id self, SEL cmd, void *a, void *b, void *c)
{
	void *result;

	entered[COUNT_finishFunnel]++;
	say("redwood: appstored: PROMISE %p finished%s\n", (void *)self, b != NULL ? " with an error" : "");
	result = old_finishFunnel(self, cmd, a, b, c);
	returned[COUNT_finishFunnel]++;
	return result;
}
WRAPQ2(finishResultError)
static void *(*old_thenWithBlock)(id, SEL, void *);
static void *
new_thenWithBlock(id self, SEL cmd, void *a)
{
	void *result;

	entered[COUNT_thenWithBlock]++;
	say("redwood: appstored: PROMISE %p waited on (then)\n", (void *)self);
	result = old_thenWithBlock(self, cmd, a);
	returned[COUNT_thenWithBlock]++;
	return result;
}
static void *(*old_continueWithBlock)(id, SEL, void *);
static void *
new_continueWithBlock(id self, SEL cmd, void *a)
{
	void *result;

	entered[COUNT_continueWithBlock]++;
	say("redwood: appstored: PROMISE %p waited on (continue)\n", (void *)self);
	result = old_continueWithBlock(self, cmd, a);
	returned[COUNT_continueWithBlock]++;
	return result;
}

WRAP1(finishWithResult)
WRAP1(finishWithError)
WRAP3(addCookies)
WRAP3(addStorefrontHeader)
WRAP3(addAnisette)
WRAP3(prepareRequest)

/* ---- installing them --------------------------------------------------------------------------- */

static struct replacement replacements[] = {
	/* the door every consumer of a storefront goes through */
	{ "ACAccount", "ams_storefront", false, (IMP)new_ams_storefront, (IMP *)&old_ams_storefront, false },
	{ "ACAccount", "ams_copyStorefrontFromAccount:", false, (IMP)new_copy_storefront, (IMP *)&old_copy_storefront, false },
	/* and the ones particular callers knock at instead */
	{ "AMSBagNetworkDataSource", "cachedStorefront", false, (IMP)new_cached_storefront, (IMP *)&old_cached_storefront, false },
	{ "AMSBagFixedAccountProvider", "bagStorefrontForAccountMediaType:", false, (IMP)new_bag_storefront,
	  (IMP *)&old_bag_storefront, false },
	{ "AMSBagActiveAccountProvider", "bagStorefrontForAccountMediaType:", false, (IMP)new_bag_storefront_active,
	  (IMP *)&old_bag_storefront_active, false },
	/* watched, not replaced */
	{ "AMSBagActiveAccountProvider", "_bagAccountForAccountMediaType:mustContainStorefront:", false,
	  (IMP)new_bag_account, (IMP *)&old_bag_account, false },
	{ "AMSBagNetworkTask", "_performFetchWithAttemptedCount:account:storefront:", false, (IMP)new_perform_fetch,
	  (IMP *)&old_perform_fetch, false },
	{ "AMSBagNetworkTask", "_queryItemsForClientInfo:profile:profileVersion:cookieNames:account:storefront:", false,
	  (IMP)new_query_items, (IMP *)&old_query_items, false },
	{ "AMSURLSession", "performRequest:completion:", false, (IMP)new_perform_request, (IMP *)&old_perform_request, false },
	{ "AMSURLSession", "performRequest:completionBlock:", false, (IMP)new_perform_request_block,
	  (IMP *)&old_perform_request_block, false },
	{ "AMSBagNetworkTask", "_createRequestWithQueryItems:", false, (IMP)new_create_request, (IMP *)&old_create_request,
	  false },
	{ "AMSBagNetworkTask", "URLSessionPromise", false, (IMP)new_session_promise, (IMP *)&old_session_promise, false },
	{ "AMSURLSession", "dataTaskPromiseWithRequest:", false, (IMP)new_data_task_promise, (IMP *)&old_data_task_promise,
	  false },
	{ "AMSURLSession", "dataTaskWithRequest:", false, (IMP)new_data_task, (IMP *)&old_data_task, false },
	{ "NSMutableURLRequest", "ams_addCookiesForAccount:clientInfo:bag:", false, (IMP)new_addCookies,
	  (IMP *)&old_addCookies, false },
	{ "NSMutableURLRequest", "ams_addStoreFrontHeaderForAccount:client:withBag:", false, (IMP)new_addStorefrontHeader,
	  (IMP *)&old_addStorefrontHeader, false },
	{ "NSMutableURLRequest", "ams_addAnisetteHeadersForAccount:type:bag:", false, (IMP)new_addAnisette,
	  (IMP *)&old_addAnisette, false },
	{ "AMSURLSession", "_prepareRequest:properties:error:", false, (IMP)new_prepareRequest,
	  (IMP *)&old_prepareRequest, false },
	{ "AMSMutablePromise", "finishWithResult:", false, (IMP)new_finishWithResult, (IMP *)&old_finishWithResult, false },
	{ "AMSMutablePromise", "finishWithError:", false, (IMP)new_finishWithError, (IMP *)&old_finishWithError, false },
	{ "AMSMutablePromise", "_finishWithResult:error:logDuplicateFinishes:", false, (IMP)new_finishFunnel,
	  (IMP *)&old_finishFunnel, false },
	{ "AMSMutablePromise", "finishWithResult:error:", false, (IMP)new_finishResultError,
	  (IMP *)&old_finishResultError, false },
	{ "AMSMutablePromise", "thenWithBlock:", false, (IMP)new_thenWithBlock, (IMP *)&old_thenWithBlock, false },
	{ "AMSMutablePromise", "continueWithBlock:", false, (IMP)new_continueWithBlock, (IMP *)&old_continueWithBlock,
	  false },
	{ "NSURLSessionTask", "resume", false, (IMP)new_resume_base, (IMP *)&old_resume_base, false },
	{ "__NSCFLocalSessionTask", "resume", false, (IMP)new_resume_local, (IMP *)&old_resume_local, false },
};

static size_t
replacement_count(void)
{
	return sizeof(replacements) / sizeof(replacements[0]);
}

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		SEL selector = sel_registerName(r->selector);
		Class super = class_getSuperclass(cls);

		method = r->class_method ? class_getClassMethod(cls, selector) : class_getInstanceMethod(cls, selector);
		if (method == NULL) {
			continue;               /* the class is there, the selector is not */
		}
		/*
		 * class_getInstanceMethod walks up: a method this class only INHERITS is the superclass's,
		 * and method_setImplementation on it would replace it for every class that inherits it as
		 * well -- for NSObject, that is every object in the daemon. appstored restarted 31 times in
		 * a run before this check was here, and said nothing on its way out (an ObjC exception ends
		 * in abort(), which the fault reporter did not watch for either; now it does).
		 */
		if (super != NULL && method == (r->class_method ? class_getClassMethod(super, selector) :
		    class_getInstanceMethod(super, selector))) {
			r->done = true;
			say("redwood: appstored: %c[%s %s] is inherited, not this class's: left alone\n",
			    r->class_method ? '+' : '-', r->class_name, r->selector);
			continue;
		}
		r->done = true;
		original = method_getImplementation(method);
		if (r->original != NULL && *r->original == NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("redwood: appstored: %c[%s %s] taken (was %#lx unslid)\n", r->class_method ? '+' : '-', r->class_name,
		    r->selector, (uintptr_t)original - slide);
	}
}

__attribute__((constructor))
static void
redwood_store_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	if (name == NULL || strcmp(name, "appstored") != 0) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	watch_for_faults();
	storefront = CFStringCreateWithCString(NULL, STOREFRONT, kCFStringEncodingUTF8);
	say("redwood: appstored: ours is in, storefront to answer with is " STOREFRONT "\n");
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, late_report, NULL) == 0) {
			pthread_detach(thread);
		}
	}
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
}
