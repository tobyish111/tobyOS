/* host stub: no process -- a link is owned by uid 0 */
struct proc { int uid, gid; };
static inline struct proc *current_proc(void) { return 0; }
