/* nsproxy.h -- Linux namespaces.
 *
 * Phase 3 slice 8 establishes the MECHANISM (CLONE_NEW* in clone/clone3,
 * unshare(2), setns(2), the /proc/PID/ns tree, refcounted namespace objects) against
 * the two cheapest payloads -- UTS (hostname/domainname) and IPC (the SysV
 * tables) -- so that slices 9-12 (mount, pid, user, net) inherit plumbing that
 * is already proven rather than debugging mechanism and payload together.
 *
 * ---------------------------------------------------------------------------
 * THE TWO DESIGN DECISIONS THAT MATTER FOR SLICES 9-12
 *
 * 1. A NULL pointer in `struct proc` means "the initial namespace".
 *
 *    This is the shape slice 5 already chose for `proc->mnt_ns`, and it is
 *    worth keeping for every namespace type: it costs no allocation at boot,
 *    it makes the fork path a no-op for the 100% of processes that never
 *    unshare anything, and it means adding a namespace type cannot change the
 *    behaviour of a system where nobody has called unshare(2).
 *
 * 2. Each namespace object carries its OWN refcount; there is no shared
 *    `struct nsproxy` indirection the way Linux has.
 *
 *    Linux's nsproxy exists to make "fork" a single refcount bump instead of
 *    seven. tobyOS's fork is already a whole-PCB memcpy followed by a fixup
 *    pass, so the bump loop costs nothing measurable and the saved indirection
 *    keeps every namespace lookup a single load. It also keeps each type
 *    independently ownable, which is what setns(2) manipulates.
 *
 * The namespace OBJECTS live with their payloads, not here: `struct uts_ns` is
 * in nsproxy.c, `struct ipc_ns` is in shm.c (next to the SysV tables it
 * contains), and `struct mount_ns` is already in vfs.c. `struct proc` holds
 * void* to each so proc.h does not drag in every subsystem's internals. That
 * is the same split slice 5 used for mnt_ns.
 * ---------------------------------------------------------------------------
 */
#ifndef TOBYOS_NSPROXY_H
#define TOBYOS_NSPROXY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct proc;
struct file;

/* Linux CLONE_NEW* bits (include/uapi/linux/sched.h). Shared by clone(2),
 * clone3(2), unshare(2) and setns(2). */
#define CLONE_NEWNS      0x00020000u
#define CLONE_NEWCGROUP  0x02000000u
#define CLONE_NEWUTS     0x04000000u
#define CLONE_NEWIPC     0x08000000u
#define CLONE_NEWUSER    0x10000000u
#define CLONE_NEWPID     0x20000000u
#define CLONE_NEWNET     0x40000000u

/* Every namespace flag Linux defines... */
#define CLONE_NEW_ANY   (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | \
                         CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | \
                         CLONE_NEWNET)
/* ...and the subset this kernel actually implements. Anything in
 * CLONE_NEW_ANY & ~CLONE_NEW_SUPPORTED is refused with -EINVAL, which is
 * precisely what Linux returns when the matching CONFIG_*_NS is off. Grow this
 * mask as slices 9-12 land; do NOT add a flag here before the namespace is
 * real, because "accepted and ignored" is how a caller ends up believing it is
 * isolated when it is not. */
#define CLONE_NEW_SUPPORTED (CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | \
                             CLONE_NEWNS  | CLONE_NEWUSER | CLONE_NEWNET)

/* Namespace kinds, in the order /proc/PID/ns lists them. */
enum {
    NS_KIND_IPC = 0,
    NS_KIND_MNT,
    NS_KIND_NET,
    NS_KIND_PID,
    NS_KIND_USER,
    NS_KIND_UTS,
    NS_KIND_COUNT
};

/* Linux __NEW_UTS_LEN (64) + 1 for the NUL. utsname fields are 65 bytes. */
#define UTS_FIELD_LEN 65

/* nsfs inode numbers -- the N in "uts:[N]".
 *
 * The initial namespaces reuse the exact constants a real Linux kernel uses
 * (PROC_*_INIT_INO, include/linux/proc_ns.h) because container tooling prints
 * these and matching costs nothing. Linux has no fixed constant for the
 * initial MNT or NET namespace (both come out of the general nsfs pool, which
 * is why real systems show a different mnt:[...] per boot), so those two get
 * stable local values instead.
 *
 * NEW namespaces are numbered from NS_INUM_DYNAMIC_BASE upward. The only
 * property anything depends on is UNIQUENESS: "same namespace" is tested by
 * comparing these numbers, so a reused inum would make two distinct
 * namespaces look identical to every tool that checks. */
#define NS_INUM_INIT_CGROUP  0xEFFFFFFBull
#define NS_INUM_INIT_PID     0xEFFFFFFCull
#define NS_INUM_INIT_USER    0xEFFFFFFDull
#define NS_INUM_INIT_UTS     0xEFFFFFFEull
#define NS_INUM_INIT_IPC     0xEFFFFFFFull
#define NS_INUM_INIT_MNT     0xF0000000ull
#define NS_INUM_INIT_NET     0xF0000001ull
#define NS_INUM_DYNAMIC_BASE 0xF0001000ull

/* Hand out the next nsfs inode number. Lives in nsproxy.c so every namespace
 * type -- including the ones shm.c and vfs.c own -- draws from one sequence. */
uint64_t ns_inum_alloc(void);

/* ---- process lifecycle ------------------------------------------------
 * nsproxy_fork_inherit() is called on a child whose PCB was memcpy'd from its
 * parent: the pointers are already correct, so all it does is take a reference
 * on each one. It MUST be called from every PCB-copying path (sys_fork,
 * sys_fork_share, sys_clone_thread) or the refcounts under-count and a live
 * namespace is freed beneath its users.
 *
 * nsproxy_release() drops this process's references and NULLs the fields, so
 * it is idempotent and safe on a path that may run twice. */
void nsproxy_fork_inherit(struct proc *child);
void nsproxy_release(struct proc *p);

/* Apply the CLONE_NEW* bits of a clone(2)/clone3(2) call to a child that has
 * already inherited the parent's namespaces. Returns 0, or a negative ABI
 * errno -- in which case the CALLER must undo the child, because the child is
 * left holding the parent's namespaces. */
long nsproxy_apply_clone_flags(struct proc *child, uint32_t flags);

/* ---- syscalls --------------------------------------------------------- */
long ns_unshare(uint32_t flags);              /* unshare(2)  */
long ns_setns(struct file *f, int nstype);    /* setns(2)    */

/* ---- UTS payload ------------------------------------------------------
 * Reads resolve against the CALLING process's UTS namespace. uname(2) and
 * sethostname(2)/setdomainname(2) are the only consumers. */
const char *uts_nodename(void);
const char *uts_domainname(void);
long uts_set_nodename(const char *name, size_t len);
long uts_set_domainname(const char *name, size_t len);

/* ---- the /proc/PID/ns tree -------------------------------------------------- */
int         ns_kind_from_name(const char *name);   /* -1 when unknown */
const char *ns_kind_name(int kind);
bool        ns_kind_implemented(int kind);
/* The nsfs inode number a namespace reports, i.e. the N in "uts:[N]". Two
 * processes are in the same namespace if and only if these match, which is
 * how `lsns`, `ip netns identify` and every container tool compare them. */
uint64_t    ns_inum_of(struct proc *p, int kind);
/* Mint a FILE_KIND_NSFD referring to `p`'s namespace of `kind`. The fd holds a
 * REFERENCE, so a namespace stays alive while an fd names it even after its
 * last member exits -- that is what makes `ip netns add` style pinning work,
 * and what makes setns(2) meaningful. NULL on OOM / unimplemented kind. */
struct file *ns_file_open(struct proc *p, int kind);

/* file.c hooks for FILE_KIND_NSFD (mirrors the eventfd/timerfd shape). */
void ns_file_clone_ref(struct file *dst, struct file *src);
void ns_file_close(struct file *f);

/* =====================================================================
 * PID namespaces (slice 10) -- src/pid_ns.c owns `struct pid_ns`.
 *
 * THE CONSTRAINT THAT SHAPES ALL OF THIS: in tobyOS a process's pid IS its
 * index into `g_proc[PROC_MAX]`, and `proc_lookup()` is a direct array index.
 * A pid therefore cannot be reassigned, and `struct proc`, the ready queues,
 * wait queues and every internal reference keep using it.
 *
 * So pids follow the shape slice 2 chose for uids: **the stored pid is always
 * the host / initial-namespace number (call it the kpid), and namespace-local
 * numbers (vpids) are produced by TRANSLATING AT THE BOUNDARY** -- at the
 * syscall reporting edge (getpid, getppid, wait's return value, siginfo,
 * procfs) and at the syscall argument edge (kill, waitpid). Nothing inside the
 * kernel ever handles a vpid. Rewriting `p->pid` instead would have meant
 * auditing every one of those internal users, which is exactly the trap slice 2
 * documented for credentials.
 *
 * A process is visible in its own namespace AND in every ancestor, which is
 * why a vpid is allocated in each of them at creation. That also makes the map
 * itself the visibility test: `pid_vnr_in(ns, kpid) == 0` means "not visible
 * from ns", with no separate ancestry walk.
 * ===================================================================== */

/* Called from every process-creation path AFTER the namespace pointers have
 * been inherited. Places `child` in the correct pid namespace -- which is
 * `parent->pid_ns_for_children`, NOT `parent->pid_ns`, because
 * unshare(CLONE_NEWPID) moves the caller's CHILDREN and not the caller -- then
 * allocates its vpid there and in every ancestor. `clone_flags` is consulted
 * for CLONE_NEWPID. Returns 0, or a negative ABI errno (caller must unwind). */
long pid_ns_place_child(struct proc *child, struct proc *parent,
                        uint32_t clone_flags, bool is_thread);

/* Release `p`'s vpid from its namespace and every ancestor. Idempotent. */
void pid_ns_forget(struct proc *p);

/* `target`'s pid as seen from the CURRENT process's namespace; 0 when the
 * target is not visible from there (which is also the right answer for
 * getppid() in a ns whose init's parent lives outside it). */
int pid_vnr(struct proc *target);
/* Same, for an explicit kpid / an explicit observer namespace. */
int pid_vnr_of_kpid(int kpid);
int pid_vnr_in(void *ns, int kpid);

/* Resolve a pid the CALLER supplied (a vpid in the caller's namespace) to a
 * kpid. 0 when no such process is visible -- callers report ESRCH. */
int pid_knr(int vpid);

/* Is `target` visible from `observer`'s pid namespace? */
bool pid_ns_can_see(struct proc *observer, struct proc *target);

/* True when `p` is the init (vpid 1) of a NON-initial pid namespace. */
bool pid_ns_is_init(struct proc *p);
/* SIGKILL every other member of `p`'s namespace. Called when a ns init exits:
 * Linux tears the whole namespace down with its init. */
void pid_ns_kill_members(struct proc *p);
/* The kpid that should adopt `dead`'s children: its namespace's init, or 0 to
 * leave the existing (initial-namespace) behaviour alone. */
int  pid_ns_reaper_kpid(struct proc *dead);

/* Lifecycle hooks used by nsproxy.c's generic dispatch. */
void    *pid_ns_create(void *parent_ns);
void     pid_ns_get(void *ns);
void     pid_ns_put(void *ns);
uint64_t pid_ns_inum(void *ns);

/* =====================================================================
 * USER namespaces (slice 11) -- src/user_ns.c owns `struct user_ns`.
 *
 * Same store-global-translate-at-the-edge shape as slices 2 and 10: `p->uid`
 * and `p->gid` stay HOST ids (kuids) forever, and only the ABI boundary
 * translates. Slice 2 wrote that decision down in advance precisely so this
 * slice would not have to touch `vfs_perm_check` and its ~30 siblings.
 * ===================================================================== */
void    *user_ns_create(void);
void     user_ns_get(void *ns);
void     user_ns_put(void *ns);
uint64_t user_ns_inum(void *ns);

/* Reporting boundary: a host id as the namespace numbers it. Unmapped ids
 * report as the overflow id (65534), never as the raw host id. */
uint32_t userns_k2v_uid(void *ns, uint32_t kuid);
uint32_t userns_k2v_gid(void *ns, uint32_t kgid);
uint32_t userns_cur_uid(uint32_t kuid);
uint32_t userns_cur_gid(uint32_t kgid);

/* Argument boundary: an id the caller supplied, as a host id. `*ok` is false
 * when there is no mapping -- callers MUST return EINVAL rather than
 * substituting anything. */
uint32_t userns_v2k_uid(uint32_t vuid, bool *ok);
uint32_t userns_v2k_gid(uint32_t vgid, bool *ok);

bool userns_is_initial(struct proc *p);
/* Does the caller hold `cap` (a Linux CAP_* number) in its user namespace? */
bool userns_capable(int cap);
/* THE ESCALATION GUARD: is the caller's mount namespace owned by the caller's
 * user namespace? Without this, unshare(CLONE_NEWUSER) alone would grant
 * CAP_SYS_ADMIN over the REAL mount table. */
bool userns_owns_mnt_ns(struct proc *p);

/* /proc/PID/{uid_map,gid_map,setgroups} */
int  userns_render_map(struct proc *p, bool want_uid, char *buf, size_t cap);
int  userns_render_setgroups(struct proc *p, char *buf, size_t cap);
long userns_write_map(struct proc *target, bool want_uid,
                      const char *buf, size_t len);
long userns_write_setgroups(struct proc *target, const char *buf, size_t len);
bool userns_setgroups_allowed(struct proc *p);

/* Which user namespace owns a mount namespace (vfs.c). NULL == initial. */
void *mount_ns_owner(void *mnt_ns);

/* =====================================================================
 * NETWORK namespaces, cut 1 (slice 12) -- src/net_ns.c.
 *
 * Cut 1 gives a new namespace NO NETWORK, by denial rather than replication:
 * this stack has no loopback at all, so an interface-less namespace genuinely
 * cannot reach anything, and none of net.c/tcp.c/ip.c's state needed to move.
 * That is what a sandbox wants and it permanently unblocks slice 14 -- but it
 * is NOT a full network namespace. Cut 2 (veth + bridge) is the refactor.
 * ===================================================================== */
void    *net_ns_create(void);
void     net_ns_get(void *ns);
void     net_ns_put(void *ns);
uint64_t net_ns_inum(void *ns);
bool     net_ns_is_initial(struct proc *p);
/* May a socket created in `sock_ns` touch a real interface? Asked of the
 * SOCKET's namespace, not the caller's, so a socket predating an unshare keeps
 * working -- as on Linux, and as an inherited socketpair must. */
bool     net_ns_has_network(void *sock_ns);

/* ---- cut 2 ---- */
struct net_dev;
struct net_dev *net_ns_dev(void *ns);
uint32_t net_ns_ip(void *ns);
uint32_t net_ns_netmask(void *ns);
uint32_t net_ns_gateway(void *ns);
void     net_ns_set_dev(void *ns, struct net_dev *dev);
/* Cut 3: a namespace holds a LIST of devices. net_ns_dev() is devs[0], the
 * primary, so every pre-cut-3 caller is unchanged. */
size_t   net_ns_dev_count(void *ns);
struct net_dev *net_ns_dev_at(void *ns, size_t i);
struct net_dev *net_ns_find_dev(void *ns, const char *name);
bool     net_ns_add_dev(void *ns, struct net_dev *dev);
bool     net_ns_del_dev(void *ns, struct net_dev *dev);
void     net_ns_set_addr(void *ns, uint32_t ip, uint32_t mask, uint32_t gw);
/* Cut 3b: per-DEVICE addressing. The per-namespace calls above answer for the
 * PRIMARY interface; RTM_NEWADDR names an interface and needs these. */
bool     net_ns_set_dev_addr(void *ns, struct net_dev *dev, uint32_t ip,
                             uint32_t mask, uint32_t gw);
uint32_t net_ns_dev_ip(void *ns, struct net_dev *dev);
uint32_t net_ns_dev_netmask(void *ns, struct net_dev *dev);
/* Cut 4: what rtnetlink needs. An interface INDEX (netlink names interfaces by
 * number -- RTM_NEWADDR carries nothing else), and an ADMIN state that veth.c
 * actually enforces. net_ns_dev_is_up() answers true for a device the namespace
 * does not hold, which is the honest answer for net.c-owned hardware. */
int      net_ns_dev_index(void *ns, struct net_dev *dev);      /* 0 == absent */
struct net_dev *net_ns_dev_by_index(void *ns, int idx);
bool     net_ns_dev_is_up(void *ns, struct net_dev *dev);
bool     net_ns_set_dev_up(void *ns, struct net_dev *dev, bool up);
uint32_t net_ns_dev_gateway(void *ns, struct net_dev *dev);
/* Bracket a frame delivery so it is processed AS `ns` (see veth.c). */
void    *net_ns_rx_enter(void *ns);
void     net_ns_rx_leave(void *prev);
void    *net_ns_rx_current(void);

/* Linux capability numbers this kernel checks by name. */
#define LCAP_SETGID     6
#define LCAP_SETUID     7
#define LCAP_NET_ADMIN  12
#define LCAP_SYS_CHROOT 18
#define LCAP_SYS_ADMIN  21

#endif /* TOBYOS_NSPROXY_H */
