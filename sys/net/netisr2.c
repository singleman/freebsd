/*-
 * Copyright (c) 2007-2009 Robert N. M. Watson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * netisr2 is a packet dispatch service, allowing synchronous (directly
 * dispatched) and asynchronous (deferred dispatch) processing of packets by
 * registered protocol handlers.  Callers pass a protocol identifier and
 * packet to netisr2, along with a direct dispatch hint, and work will either
 * be immediately processed with the registered handler, or passed to a
 * kernel worker thread for deferred dispatch.
 *
 * Maintaining ordering for protocol streams is a critical design concern.
 * Enforcing ordering limits the opportunity for concurrency, but maintains
 * the strong ordering requirements found in some protocols, such as TCP.  Of
 * related concern is CPU affinity--it is desirable to process all data
 * associated with a particular stream on the same CPU over time in order to
 * avoid acquiring locks associated with the connection on different CPUs,
 * keep connection data in one cache, and to generally encourage associated
 * user threads to live on the same CPU as the stream.  It's also desirable
 * to avoid lock migration and contention where locks are associated with
 * more than one flow.
 *
 * There are two cases:
 *
 * - The packet has a flow ID, query the protocol to map it to a CPU and
 *   execute there if not direct dispatching.
 *
 * - The packet has no flowid, query the protocol to generate a flow ID, then
 *   query a CPU and execute there if not direct dispatching.
 *
 * We guarantee that if two packets from the same source have the same
 * protocol, and the source provides an ordering, that ordering will be
 * maintained *unless* the policy is changing between queued and direct
 * dispatch in which case minor re-ordering might occur.
 *
 * Some possible sources of flow identifiers for packets:
 * - Hardware-generated hash from RSS
 * - Software-generated hash from addresses and ports identifying the flow
 * - Interface identifier the packet came from
 */

#include "opt_ddb.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/interrupt.h>
#include <sys/lock.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#ifdef DDB
#include <ddb/ddb.h>
#endif

#include <net/netisr.h>
#include <net/netisr2.h>

/*-
 * Synchronize use and modification of the registered netisr data structures;
 * acquire a read lock while modifying the set of registered protocols to
 * prevent partially registered or unregistered protocols from being run.
 *
 * We make per-packet use optional so that we can measure the performance
 * impact of providing consistency against run-time registration and
 * deregristration, which is a very uncommon event.
 *
 * The following data structures and fields are protected by this lock:
 *
 * - The np array, including all fields of struct netisr_proto.
 * - The nws array, including all fields of struct netisr_worker.
 * - The nws_array array.
 *
 * XXXRW: This should use an rmlock.
 */
static struct rwlock	netisr_rwlock;
#define	NETISR_LOCK_INIT()	rw_init(&netisr_rwlock, "netisr")
#ifdef NETISR_LOCKING
#define	NETISR_LOCK_ASSERT()	rw_assert(&netisr_rwlock, RW_LOCKED)
#define	NETISR_RLOCK()		rw_rlock(&netisr_rwlock)
#define	NETISR_RUNLOCK()	rw_runlock(&netisr_rwlock)
#else
#define	NETISR_LOCK_ASSERT()
#define	NETISR_RLOCK()
#define	NETISR_RUNLOCK()
#endif
#define	NETISR_WLOCK()		rw_wlock(&netisr_rwlock)
#define	NETISR_WUNLOCK()	rw_wunlock(&netisr_rwlock)

SYSCTL_NODE(_net, OID_AUTO, isr2, CTLFLAG_RW, 0, "netisr2");

static int	netisr_direct = 1;	/* Enable direct dispatch. */
SYSCTL_INT(_net_isr2, OID_AUTO, direct, CTLFLAG_RW, &netisr_direct, 0,
    "Direct dispatch");

/*
 * Allow the administrator to limit the number of threads (CPUs) to use for
 * netisr2.  Notice that we don't check netisr_maxthreads before creating the
 * thread for CPU 0, so in practice we ignore values <= 1.  This must be set
 * as a tunable, no run-time reconfiguration yet.
 */
static int	netisr_maxthreads = MAXCPU;	/* Bound number of threads. */
TUNABLE_INT("net.isr2.maxthreads", &netisr_maxthreads);
SYSCTL_INT(_net_isr2, OID_AUTO, maxthreads, CTLFLAG_RD, &netisr_maxthreads,
    0, "Use at most this many CPUs for netisr2 processing");

/*
 * Each protocol is described by an instance of netisr_proto, which holds all
 * global per-protocol information.  This data structure is set up by
 * netisr_register().
 */
struct netisr_proto {
	const char		*np_name;	/* Protocol name. */
	netisr_t		*np_func;	/* Protocol handler. */
	netisr_m2flow_t		*np_m2flow;	/* mbuf -> flow ID. */
	netisr_flow2cpu_t	*np_flow2cpu;	/* Flow ID -> CPU ID. */
};

#define	NETISR_MAXPROT		32		/* Compile-time limit. */
#define	NETISR_ALLPROT		0xffffffff	/* Run all protocols. */

/*
 * The np array describes all registered protocols, indexed by protocol
 * number.
 */
static struct netisr_proto	np[NETISR_MAXPROT];

/*
 * Protocol-specific work for each workstream is described by struct
 * netisr_work.  Each work descriptor consists of an mbuf queue and
 * statistics.
 *
 * XXXRW: Using a lock-free linked list here might be useful.
 */
struct netisr_work {
	/*
	 * Packet queue, linked by m_nextpkt.
	 */
	struct mbuf	*nw_head;
	struct mbuf	*nw_tail;
	u_int		 nw_len;
	u_int		 nw_max;
	u_int		 nw_watermark;

	/*
	 * Statistics -- written unlocked, but mostly from curcpu.
	 */
	u_int		 nw_dispatched; /* Number of direct dispatches. */
	u_int		 nw_dropped;	/* Number of drops. */
	u_int		 nw_queued;	/* Number of enqueues. */
	u_int		 nw_handled;	/* Number passed into handler. */
};

/*
 * Workstreams hold a set of ordered work across each protocol, and are
 * described by netisr_workstream.  Each workstream is associated with a
 * worker thread, which in turn is pinned to a CPU.  Work associated with a
 * workstream can be processd in other threads during direct dispatch;
 * concurrent processing is prevented by the NWS_RUNNING flag, which
 * indicates that a thread is already processing the work queue.
 *
 * Currently, #workstreams must equal #CPUs.
 */
struct netisr_workstream {
	struct thread	*nws_thread;		/* Thread serving stream. */
	struct mtx	 nws_mtx;		/* Synchronize work. */
	struct cv	 nws_cv;		/* Wake up worker. */
	u_int		 nws_cpu;		/* CPU pinning. */
	u_int		 nws_flags;		/* Wakeup flags. */

	u_int		 nws_pendingwork;	/* Across all protos. */
	/*
	 * Each protocol has per-workstream data.
	 */
	struct netisr_work	nws_work[NETISR_MAXPROT];
} __aligned(CACHE_LINE_SIZE);

/*
 * Kernel process associated with worker threads.
 */
static struct proc			*netisr2_proc;

/*
 * Per-CPU workstream data, indexed by CPU ID.
 */
static struct netisr_workstream		 nws[MAXCPU];

/*
 * Map contiguous values between 0 and nws_count into CPU IDs appropriate for
 * indexing the nws[] array.  This allows constructions of the form
 * nws[nws_array(arbitraryvalue % nws_count)].
 */
static u_int				 nws_array[MAXCPU];

/*
 * Number of registered workstreams.  Should be the number of running CPUs
 * once fully started.
 */
static u_int				 nws_count;

/*
 * Per-workstream flags.
 */
#define	NWS_RUNNING	0x00000001	/* Currently running in a thread. */
#define	NWS_SIGNALED	0x00000002	/* Signal issued. */

/*
 * Synchronization for each workstream: a mutex protects all mutable fields
 * in each stream, including per-protocol state (mbuf queues).  The CV will
 * be used to wake up the worker if asynchronous dispatch is required.
 */
#define	NWS_LOCK(s)		mtx_lock(&(s)->nws_mtx)
#define	NWS_LOCK_ASSERT(s)	mtx_assert(&(s)->nws_mtx, MA_OWNED)
#define	NWS_UNLOCK(s)		mtx_unlock(&(s)->nws_mtx)
#define	NWS_SIGNAL(s)		cv_signal(&(s)->nws_cv)
#define	NWS_WAIT(s)		cv_wait(&(s)->nws_cv, &(s)->nws_mtx)

/*
 * Utility routines for protocols that implement their own mapping of flows
 * to CPUs.
 */
u_int
netisr2_get_cpucount(void)
{

	return (nws_count);
}

u_int
netisr2_get_cpuid(u_int cpunumber)
{

	return (nws_array[cpunumber]);
}

/*
 * The default implementation of (source, flow ID) -> CPU ID mapping.
 * Non-static so that protocols can use it to map their own work to specific
 * CPUs in a manner consistent to netisr2 for affinity purposes.
 */
u_int
netisr2_default_flow2cpu(uintptr_t source, u_int flowid)
{

	return (netisr2_get_cpuid((source ^ flowid) %
	    netisr2_get_cpucount()));
}

/*
 * Register a new netisr handler, which requires initializing per-protocol
 * fields for each workstream.  All netisr2 work is briefly suspended while
 * the protocol is installed.
 */
void
netisr2_register(u_int proto, const char *name, netisr_t func,
    netisr_m2flow_t m2flow, netisr_flow2cpu_t flow2cpu, u_int max)
{
	struct netisr_work *npwp;
	int i;

	NETISR_WLOCK();
	KASSERT(proto < NETISR_MAXPROT,
	    ("netisr2_register(%d, %s): too many protocols", proto, name));
	KASSERT(np[proto].np_name == NULL,
	    ("netisr2_register(%d, %s): name present", proto, name));
	KASSERT(np[proto].np_func == NULL,
	    ("netisr2_register(%d, %s): func present", proto, name));
	KASSERT(np[proto].np_m2flow == NULL,
	    ("netisr2_register(%d, %s): m2flow present", proto, name));
	KASSERT(np[proto].np_flow2cpu == NULL,
	    ("netisr2_register(%d, %s): flow2cpu present", proto, name));

	KASSERT(name != NULL, ("netisr2_register: name NULL for %d", proto));
	KASSERT(func != NULL, ("netisr2_register: func NULL for %s", name));

	/*
	 * Initialize global and per-workstream protocol state.
	 */
	np[proto].np_name = name;
	np[proto].np_func = func;
	np[proto].np_m2flow = m2flow;
	if (flow2cpu != NULL)
		np[proto].np_flow2cpu = flow2cpu;
	else
		np[proto].np_flow2cpu = netisr2_default_flow2cpu;
	for (i = 0; i < MAXCPU; i++) {
		npwp = &nws[i].nws_work[proto];
		bzero(npwp, sizeof(*npwp));
		npwp->nw_max = max;
	}
	NETISR_WUNLOCK();
}

/*
 * Drain all packets currently held in a particular protocol work queue.
 */
static void
netisr2_drain_proto(struct netisr_work *npwp)
{
	struct mbuf *m;

	while ((m = npwp->nw_head) != NULL) {
		npwp->nw_head = m->m_nextpkt;
		m->m_nextpkt = NULL;
		if (npwp->nw_head == NULL)
			npwp->nw_tail = NULL;
		npwp->nw_len--;
		m_freem(m);
	}
	KASSERT(npwp->nw_tail == NULL, ("netisr_drain_proto: tail"));
	KASSERT(npwp->nw_len == 0, ("netisr_drain_proto: len"));
}

/*
 * Remove the registration of a network protocol, which requires clearing
 * per-protocol fields across all workstreams, including freeing all mbufs in
 * the queues at time of deregister.  All work in netisr2 is briefly
 * suspended while this takes place.
 */
void
netisr2_deregister(u_int proto)
{
	struct netisr_work *npwp;
	int i;

	NETISR_WLOCK();
	KASSERT(proto < NETISR_MAXPROT,
	    ("netisr_deregister(%d): protocol too big", proto));
	KASSERT(np[proto].np_func != NULL,
	    ("netisr_deregister(%d): protocol not registered", proto));

	np[proto].np_name = NULL;
	np[proto].np_func = NULL;
	np[proto].np_m2flow = NULL;
	np[proto].np_flow2cpu = NULL;
	for (i = 0; i < MAXCPU; i++) {
		npwp = &nws[i].nws_work[proto];
		netisr2_drain_proto(npwp);
		bzero(npwp, sizeof(*npwp));
	}
	NETISR_WUNLOCK();
}

/*
 * Look up the correct stream for a requested flowid.  There are two cases:
 * one in which the caller has requested execution on the current CPU (i.e.,
 * source ordering is sufficient, perhaps because the underlying hardware has
 * generated multiple input queues with sufficient order), or the case in
 * which we ask the protocol to generate a flowid.  In the latter case, we
 * rely on the protocol generating a reasonable distribution across the
 * flowid space, and hence use a very simple mapping from flowids to workers.
 *
 * Because protocols may need to call m_pullup(), they may rewrite parts of
 * the mbuf chain.  As a result, we must return an mbuf chain that is either
 * the old chain (if there is no update) or the new chain (if there is). NULL
 * is returned if there is a failure in the protocol portion of the lookup
 * (i.e., out of mbufs and a rewrite is required).
 */
static struct mbuf *
netisr2_selectcpu(struct netisr_proto *npp, uintptr_t source, struct mbuf *m,
    u_int *cpuidp)
{

	NETISR_LOCK_ASSERT();

	if (!(m->m_flags & M_FLOWID) && npp->np_m2flow != NULL) {
		m = npp->np_m2flow(m);
		if (m == NULL)
			return (NULL);
		KASSERT(m->m_flags & M_FLOWID, ("netisr2_selectcpu: protocol"
		    " %s failed to return flowid on mbuf",
		    npp->np_name));
	}
	if (m->m_flags & M_FLOWID)
		*cpuidp = npp->np_flow2cpu(source, m->m_pkthdr.flowid);
	else
		*cpuidp = npp->np_flow2cpu(source, 0);
	return (m);
}

/*
 * Process packets associated with a workstream and protocol.  For reasons of
 * fairness, we process up to one complete netisr queue at a time, moving the
 * queue to a stack-local queue for processing, but do not loop refreshing
 * from the global queue.  The caller is responsible for deciding whether to
 * loop, and for setting the NWS_RUNNING flag.  The passed workstream will be
 * locked on entry and relocked before return, but will be released while
 * processing.
 */
static void
netisr2_process_workstream_proto(struct netisr_workstream *nwsp, int proto)
{
	struct netisr_work local_npw, *npwp;
	u_int handled;
	struct mbuf *m;

	NWS_LOCK_ASSERT(nwsp);

	KASSERT(nwsp->nws_flags & NWS_RUNNING,
	    ("netisr_process_workstream_proto(%d): not running", proto));
	KASSERT(proto >= 0 && proto < NETISR_MAXPROT,
	    ("netisr_process_workstream_proto(%d): invalid proto\n", proto));

	npwp = &nwsp->nws_work[proto];
	if (npwp->nw_len == 0)
		return;

	/*
	 * Move the global work queue to a thread-local work queue.
	 *
	 * Notice that this means the effective maximum length of the queue
	 * is actually twice that of the maximum queue length specified in
	 * the protocol registration call.
	 */
	handled = npwp->nw_len;
	local_npw = *npwp;
	npwp->nw_head = NULL;
	npwp->nw_tail = NULL;
	npwp->nw_len = 0;
	nwsp->nws_pendingwork -= handled;
	NWS_UNLOCK(nwsp);
	while ((m = local_npw.nw_head) != NULL) {
		local_npw.nw_head = m->m_nextpkt;
		m->m_nextpkt = NULL;
		if (local_npw.nw_head == NULL)
			local_npw.nw_tail = NULL;
		local_npw.nw_len--;
		np[proto].np_func(m);
	}
	KASSERT(local_npw.nw_len == 0,
	    ("netisr_process_proto(%d): len %d", proto, local_npw.nw_len));
	NWS_LOCK(nwsp);
	npwp->nw_handled += handled;
}

/*
 * Process either one or all protocols associated with a specific workstream.
 * Handle only existing work for each protocol processed, not new work that
 * may arrive while processing.  Set the running flag so that other threads
 * don't also try to process work in the queue; however, the lock on the
 * workstream will be released by netisr_process_workstream_proto() while
 * entering the protocol so that producers can continue to queue new work.
 *
 * The consumer is responsible for making sure that either all available work
 * is performed until there is no more work to perform, or that the worker is
 * scheduled to pick up where the consumer left off.  They are also
 * responsible for checking the running flag before entering this function.
 */
static void
netisr2_process_workstream(struct netisr_workstream *nwsp, int proto)
{
	u_int i;

	NETISR_LOCK_ASSERT();
	NWS_LOCK_ASSERT(nwsp);

	KASSERT(nwsp->nws_flags & NWS_RUNNING,
	    ("netisr2_process_workstream: not running"));
	if (proto == NETISR_ALLPROT) {
		for (i = 0; i < NETISR_MAXPROT; i++)
			netisr2_process_workstream_proto(nwsp, i);
	} else
		netisr2_process_workstream_proto(nwsp, proto);
}

/*
 * Worker thread that waits for and processes packets in a set of workstreams
 * that it owns.  Each thread has one cv, which is uses for all workstreams
 * it handles.
 */
static void
netisr2_worker(void *arg)
{
	struct netisr_workstream *nwsp;

	nwsp = arg;

	thread_lock(curthread);
	sched_prio(curthread, SWI_NET * RQ_PPQ + PI_SOFT);
	sched_bind(curthread, nwsp->nws_cpu);
	thread_unlock(curthread);

	/*
	 * Main work loop.  In the future we will want to support stopping
	 * workers, as well as re-balancing work, in which case we'll need to
	 * also handle state transitions.
	 *
	 * XXXRW: netisr_rwlock.
	 */
	NWS_LOCK(nwsp);
	while (1) {
		while (nwsp->nws_pendingwork == 0) {
			nwsp->nws_flags &= ~(NWS_SIGNALED | NWS_RUNNING);
			NWS_WAIT(nwsp);
			nwsp->nws_flags |= NWS_RUNNING;
		}
		netisr2_process_workstream(nwsp, NETISR_ALLPROT);
	}
}

static int
netisr2_queue_internal(u_int proto, struct mbuf *m, u_int cpuid)
{
	struct netisr_workstream *nwsp;
	struct netisr_work *npwp;
	int dosignal, error;

	NETISR_LOCK_ASSERT();

	dosignal = 0;
	error = 0;
	nwsp = &nws[cpuid];
	npwp = &nwsp->nws_work[proto];
	NWS_LOCK(nwsp);
	if (npwp->nw_len < npwp->nw_max) {
		m->m_nextpkt = NULL;
		if (npwp->nw_head == NULL) {
			npwp->nw_head = m;
			npwp->nw_tail = m;
		} else {
			npwp->nw_tail->m_nextpkt = m;
			npwp->nw_tail = m;
		}
		npwp->nw_len++;
		if (npwp->nw_len > npwp->nw_watermark)
			npwp->nw_watermark = npwp->nw_len;
		nwsp->nws_pendingwork++;
		if (!(nwsp->nws_flags & NWS_SIGNALED)) {
			nwsp->nws_flags |= NWS_SIGNALED;
			dosignal = 1;	/* Defer until unlocked. */
		}
		error = 0;
	} else
		error = ENOBUFS;
	NWS_UNLOCK(nwsp);
	if (dosignal)
		NWS_SIGNAL(nwsp);
	if (error)
		npwp->nw_dropped++;
	else
		npwp->nw_queued++;
	return (error);
}

int
netisr2_queue(u_int proto, uintptr_t source, struct mbuf *m)
{
	u_int cpuid, error;

	KASSERT(proto < NETISR_MAXPROT,
	    ("netisr2_dispatch: invalid proto %d", proto));

	NETISR_RLOCK();
	KASSERT(np[proto].np_func != NULL,
	    ("netisr2_dispatch: invalid proto %d", proto));

	m = netisr2_selectcpu(&np[proto], source, m, &cpuid);
	if (m != NULL)
		error = netisr2_queue_internal(proto, m, cpuid);
	else
		error = ENOBUFS;
	NETISR_RUNLOCK();
	return (error);
}

int
netisr2_dispatch(u_int proto, uintptr_t source, struct mbuf *m)
{
	struct netisr_workstream *nwsp;
	struct netisr_work *npwp;

	if (!netisr_direct)
		return (netisr2_queue(proto, source, m));
	KASSERT(proto < NETISR_MAXPROT,
	    ("netisr2_dispatch: invalid proto %d", proto));

	NETISR_RLOCK();
	KASSERT(np[proto].np_func != NULL,
	    ("netisr2_dispatch: invalid proto %d", proto));

	/*
	 * Borrow current CPU's stats, even if there's no worker.
	 */
	nwsp = &nws[curcpu];
	npwp = &nwsp->nws_work[proto];
	npwp->nw_dispatched++;
	npwp->nw_handled++;
	np[proto].np_func(m);
	NETISR_RUNLOCK();
	return (0);
}

/*
 * Initialize the netisr subsystem.  We rely on BSS and static initialization
 * of most fields in global data structures.
 *
 * Start a worker thread for the boot CPU so that we can support network
 * traffic immediately in case the netowrk stack is used before additional
 * CPUs are started (for example, diskless boot).
 */
static void
netisr2_init(void *arg)
{
	struct netisr_workstream *nwsp;
	u_int cpuid;
	int error;

	KASSERT(curcpu == 0, ("netisr2_init: not on CPU 0"));

	NETISR_LOCK_INIT();

	KASSERT(PCPU_GET(netisr2) == NULL, ("netisr2_init: pc_netisr2"));

	cpuid = curcpu;
	nwsp = &nws[cpuid];
	mtx_init(&nwsp->nws_mtx, "netisr2_mtx", NULL, MTX_DEF);
	cv_init(&nwsp->nws_cv, "netisr2_cv");
	nwsp->nws_cpu = cpuid;
	error = kproc_kthread_add(netisr2_worker, nwsp, &netisr2_proc,
	    &nwsp->nws_thread, 0, 0, "netisr2", "netisr2: cpu%d", cpuid);
	PCPU_SET(netisr2, nwsp->nws_thread);
	if (error)
		panic("netisr2_init: kproc_kthread_add %d", error);
	nws_array[nws_count] = nwsp->nws_cpu;
	nws_count++;
	if (netisr_maxthreads < 1)
		netisr_maxthreads = 1;
}
SYSINIT(netisr2_init, SI_SUB_SOFTINTR, SI_ORDER_FIRST, netisr2_init, NULL);

/*
 * Start worker threads for additional CPUs.  No attempt to gracefully handle
 * work reassignment, we don't yet support dynamic reconfiguration.
 */
static void
netisr2_start(void *arg)
{
	struct netisr_workstream *nwsp;
	struct pcpu *pc;
	int error;

	SLIST_FOREACH(pc, &cpuhead, pc_allcpu) {
		if (nws_count >= netisr_maxthreads)
			break;
		/* XXXRW: Is skipping absent CPUs still required here? */
		if (CPU_ABSENT(pc->pc_cpuid))
			continue;
		/* Worker will already be present for boot CPU. */
		if (pc->pc_netisr2 != NULL)
			continue;
		nwsp = &nws[pc->pc_cpuid];
		mtx_init(&nwsp->nws_mtx, "netisr2_mtx", NULL, MTX_DEF);
		cv_init(&nwsp->nws_cv, "netisr2_cv");
		nwsp->nws_cpu = pc->pc_cpuid;
		error = kproc_kthread_add(netisr2_worker, nwsp,
		    &netisr2_proc, &nwsp->nws_thread, 0, 0, "netisr2",
		    "netisr2: cpu%d", pc->pc_cpuid);
		pc->pc_netisr2 = nwsp->nws_thread;
		if (error)
			panic("netisr2_start: kproc_kthread_add %d", error);
		nws_array[nws_count] = pc->pc_cpuid;
		nws_count++;
	}
}
SYSINIT(netisr2_start, SI_SUB_SMP, SI_ORDER_MIDDLE, netisr2_start, NULL);

#ifdef DDB
DB_SHOW_COMMAND(netisr2, db_show_netisr2)
{
	struct netisr_workstream *nwsp;
	struct netisr_work *nwp;
	int cpu, first, proto;

	db_printf("%6s %6s %6s %6s %6s %6s %8s %8s %8s %8s\n", "CPU", "Pend",
	    "Proto", "Len", "WMark", "Max", "Disp", "Drop", "Queue", "Handle");
	for (cpu = 0; cpu < MAXCPU; cpu++) {
		nwsp = &nws[cpu];
		if (nwsp->nws_thread == NULL)
			continue;
		first = 1;
		for (proto = 0; proto < NETISR_MAXPROT; proto++) {
			if (np[proto].np_func == NULL)
				continue;
			nwp = &nwsp->nws_work[proto];
			if (first) {
				db_printf("%6d %6d ", cpu,
				    nwsp->nws_pendingwork);
				first = 0;
			} else
				db_printf("%6s %6s ", "", "");
			db_printf("%6s %6d %6d %6d %8d %8d %8d %8d\n",
			    np[proto].np_name, nwp->nw_len,
			    nwp->nw_watermark, nwp->nw_max,
			    nwp->nw_dispatched, nwp->nw_dropped,
			    nwp->nw_queued, nwp->nw_handled);
		}
	}
}
#endif
