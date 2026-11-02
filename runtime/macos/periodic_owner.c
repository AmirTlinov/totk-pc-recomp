#include "periodic_owner.h"
#include <stdlib.h>
#include <string.h>

typedef struct OwnerBinding OwnerBinding;
struct PeriodicOwnerProgram {
    struct PeriodicOwnerProgram *next;
    PeriodicOwnerGraph *graph;
    uint64_t id, implementation;
    size_t action_count, port_count;
    PeriodicOwnerAction *actions;
    PeriodicOwnerPort *ports;
};
struct PeriodicDependency {
    struct PeriodicDependency *next;
    PeriodicOwnerGraph *graph;
    uint64_t identity, generation;
    uint64_t validation_visit, validation_version;
    OwnerBinding *users;
};
struct OwnerBinding {
    PeriodicParticipation *owner;
    PeriodicDependency *dependency;
    OwnerBinding *next, **previous;
    uint64_t entry_version, cycle_version, exit_version;
    unsigned returned;
};
struct PeriodicParticipation {
    // Hot state belongs to the participation, not its shared program.
    const PeriodicOwnerProgram *program;
    PeriodicCausalEntry entry;
    uint64_t revision, invocation;
    size_t next_action;
    PeriodicOwnerStatus status;
    unsigned closed;
    PeriodicCausalEntry cycle_entry;
    OwnerBinding *bindings;
    PeriodicParticipation *next, **previous;
    PeriodicParticipation *work_next;
    uint64_t visited;
};
struct PeriodicOwnerGraph {
    PeriodicOwnerProgram *programs;
    PeriodicDependency *dependencies;
    PeriodicParticipation *owners;
    uint64_t next_program, next_revision, traversal;
};

static int same_entry(PeriodicCausalEntry a, PeriodicCausalEntry b) {
    return a.owner == b.owner && a.generation == b.generation &&
        a.role == b.role && a.phase == b.phase &&
        a.parameter_generation == b.parameter_generation;
}
static int bound(const PeriodicParticipation *o) {
    for (size_t i = 0; i < o->program->port_count; ++i)
        if (!o->bindings[i].dependency) return 0;
    return 1;
}
static int versions_match(const PeriodicParticipation *o) {
    for (size_t i = 0; i < o->program->port_count; ++i) {
        const OwnerBinding *b = &o->bindings[i];
        if (!b->dependency || b->dependency->generation != b->entry_version) return 0;
    }
    return 1;
}
static void capture_entry(PeriodicParticipation *o) {
    o->cycle_entry = o->entry;
    for (size_t i = 0; i < o->program->port_count; ++i) {
        o->bindings[i].entry_version = o->bindings[i].dependency->generation;
        o->bindings[i].cycle_version = o->bindings[i].entry_version;
        o->bindings[i].exit_version = o->bindings[i].entry_version;
        o->bindings[i].returned = 0;
    }
}
static void unlink_binding(OwnerBinding *b) {
    if (b->previous) {
        *b->previous = b->next;
        if (b->next) b->next->previous = b->previous;
    }
    b->next = NULL; b->previous = NULL; b->dependency = NULL;
}

PeriodicOwnerGraph *periodic_owner_graph_create(void) { return calloc(1, sizeof(PeriodicOwnerGraph)); }
void periodic_owner_destroy(PeriodicParticipation *o) {
    if (!o) return;
    // Removing a writer is an external intervention for its consumers.
    periodic_owner_contact(o);
    for (size_t i = 0; i < o->program->port_count; ++i) unlink_binding(&o->bindings[i]);
    *o->previous = o->next;
    if (o->next) o->next->previous = o->previous;
    free(o->bindings); free(o);
}
void periodic_owner_graph_destroy(PeriodicOwnerGraph *g) {
    if (!g) return;
    while (g->owners) periodic_owner_destroy(g->owners);
    while (g->programs) {
        PeriodicOwnerProgram *p = g->programs; g->programs = p->next;
        free(p->actions); free(p->ports); free(p);
    }
    while (g->dependencies) {
        PeriodicDependency *d = g->dependencies; g->dependencies = d->next; free(d);
    }
    free(g);
}
const PeriodicOwnerProgram *periodic_owner_program(PeriodicOwnerGraph *g,
    uint64_t implementation, const PeriodicOwnerAction *actions, size_t n,
    const PeriodicOwnerPort *ports, size_t m) {
    if (!g || !implementation || !actions || !n || (m && !ports) ||
        n > SIZE_MAX / sizeof *actions || m > SIZE_MAX / sizeof *ports) return NULL;
    for (size_t i = 0; i < n; ++i) {
        if (actions[i].exit_phase != actions[(i + 1) % n].entry_phase) return NULL;
        // This renderer model has one fixed participation role. Role routing
        // needs an explicit binding correspondence; a changing role must not
        // release another live participation's identity halfway through a cycle.
        if (actions[i].role != actions[0].role) return NULL;
        if (actions[i].first_port > m || actions[i].port_count > m - actions[i].first_port) return NULL;
        for (size_t j = 0; j < i; ++j)
            if (actions[i].entry_phase == actions[j].entry_phase && actions[i].role == actions[j].role) return NULL;
    }
    for (size_t i = 0; i < m; ++i)
        if (!ports[i].access || (ports[i].access & ~3u)) return NULL;
    for (size_t port = 0; port < m; ++port) {
        int owed = 0;
        for (size_t i = 0; i < n; ++i)
            owed |= port >= actions[i].first_port && port - actions[i].first_port < actions[i].port_count;
        if (!owed) return NULL;
    }
    for (PeriodicOwnerProgram *p = g->programs; p; p = p->next) {
        if (p->implementation != implementation || p->action_count != n || p->port_count != m) continue;
        size_t i = 0;
        for (; i < n; ++i)
            if (p->actions[i].entry_phase != actions[i].entry_phase ||
                p->actions[i].exit_phase != actions[i].exit_phase || p->actions[i].role != actions[i].role ||
                p->actions[i].first_port != actions[i].first_port || p->actions[i].port_count != actions[i].port_count) break;
        if (i != n) continue;
        for (i = 0; i < m; ++i)
            if (p->ports[i].role != ports[i].role || p->ports[i].access != ports[i].access) break;
        if (i == m) return p;
    }
    PeriodicOwnerProgram *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->actions = malloc(n * sizeof *actions);
    p->ports = m ? malloc(m * sizeof *ports) : NULL;
    if (!p->actions || (m && !p->ports)) { free(p->actions); free(p->ports); free(p); return NULL; }
    memcpy(p->actions, actions, n * sizeof *actions);
    if (m) memcpy(p->ports, ports, m * sizeof *ports);
    p->graph = g; p->implementation = implementation; p->id = ++g->next_program;
    p->action_count = n; p->port_count = m; p->next = g->programs; g->programs = p;
    return p;
}
uint64_t periodic_owner_program_id(const PeriodicOwnerProgram *p) { return p ? p->id : 0; }
PeriodicParticipation *periodic_owner_create(PeriodicOwnerGraph *g,
    const PeriodicOwnerProgram *p, PeriodicCausalEntry entry) {
    if (!g || !p || p->graph != g || !entry.owner || !entry.generation) return NULL;
    for (PeriodicParticipation *other = g->owners; other; other = other->next)
        if (other->entry.owner == entry.owner && other->entry.generation == entry.generation &&
            other->entry.role == entry.role) return NULL;
    size_t next = 0;
    while (next < p->action_count && (p->actions[next].entry_phase != entry.phase ||
            p->actions[next].role != entry.role)) ++next;
    if (next == p->action_count) return NULL;
    PeriodicParticipation *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->bindings = p->port_count ? calloc(p->port_count, sizeof *o->bindings) : NULL;
    if (p->port_count && !o->bindings) { free(o); return NULL; }
    o->program = p; o->entry = o->cycle_entry = entry; o->next_action = next;
    o->revision = ++g->next_revision; o->status = p->port_count ? PERIODIC_OWNER_REVISED : PERIODIC_OWNER_READY;
    o->previous = &g->owners; o->next = g->owners;
    if (o->next) o->next->previous = &o->next;
    g->owners = o;
    for (size_t i = 0; i < p->port_count; ++i) o->bindings[i].owner = o;
    return o;
}
PeriodicDependency *periodic_owner_dependency(PeriodicOwnerGraph *g, uint64_t id, uint64_t version) {
    if (!g || !id || !version) return NULL;
    for (PeriodicDependency *d = g->dependencies; d; d = d->next)
        if (d->identity == id) return d->generation == version ? d : NULL;
    PeriodicDependency *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->graph = g; d->identity = id; d->generation = version;
    d->next = g->dependencies; g->dependencies = d;
    return d;
}
int periodic_owner_dependency_destroy(PeriodicDependency *d) {
    if (!d || d->users) return 0;
    PeriodicDependency **at = &d->graph->dependencies;
    while (*at != d) at = &(*at)->next;
    *at = d->next; free(d); return 1;
}

// Intrusive work links make invalidation proportional to the affected cone.
// Writers are revisited once even if the dependency graph contains a cycle.
static void enqueue(PeriodicParticipation **head, PeriodicParticipation *o, uint64_t visit) {
    if (o->visited == visit) return;
    o->visited = visit; o->work_next = *head; *head = o;
}
static void revise_cone(PeriodicParticipation *head, uint64_t visit) {
    while (head) {
        PeriodicParticipation *o = head; head = o->work_next; o->work_next = NULL;
        o->revision = ++o->program->graph->next_revision;
        o->closed = 0; o->status = PERIODIC_OWNER_REVISED;
        for (size_t i = 0; i < o->program->port_count; ++i) {
            OwnerBinding *b = &o->bindings[i]; b->returned = 0;
            if (!(o->program->ports[i].access & PERIODIC_OWNER_WRITE) || !b->dependency) continue;
            for (OwnerBinding *r = b->dependency->users; r; r = r->next)
                enqueue(&head, r->owner, visit);
        }
    }
}
void periodic_owner_contact(PeriodicParticipation *o) {
    if (!o) return;
    PeriodicParticipation *head = NULL;
    uint64_t visit = ++o->program->graph->traversal;
    enqueue(&head, o, visit); revise_cone(head, visit);
}
void periodic_owner_dependency_change(PeriodicDependency *d, uint64_t generation) {
    if (!d || !generation || d->generation == generation) return;
    d->generation = generation;
    PeriodicParticipation *head = NULL;
    uint64_t visit = ++d->graph->traversal;
    for (OwnerBinding *b = d->users; b; b = b->next) enqueue(&head, b->owner, visit);
    revise_cone(head, visit);
}
int periodic_owner_bind(PeriodicParticipation *o, size_t port, PeriodicDependency *d) {
    if (!o || !d || port >= o->program->port_count || d->graph != o->program->graph) return 0;
    OwnerBinding *b = &o->bindings[port];
    if (b->dependency == d) return 1;
    periodic_owner_contact(o); unlink_binding(b);
    b->dependency = d; b->previous = &d->users; b->next = d->users;
    if (b->next) b->next->previous = &b->next;
    d->users = b; return 1;
}
int periodic_owner_revalidate(PeriodicParticipation *o, uint64_t parameter_generation) {
    if (!o || !parameter_generation || o->status == PERIODIC_OWNER_OPEN || !bound(o)) return 0;
    if (o->status == PERIODIC_OWNER_READY && o->entry.parameter_generation == parameter_generation && versions_match(o)) return 1;
    periodic_owner_contact(o);
    o->entry.parameter_generation = parameter_generation;
    o->status = PERIODIC_OWNER_READY; capture_entry(o); return 1;
}
static int owns(const PeriodicParticipation *o, const PeriodicOwnerInvocation *i) {
    return o && i && i->participation == o && o->status == PERIODIC_OWNER_OPEN &&
        i->invocation == o->invocation && i->revision == o->revision && same_entry(i->entry, o->entry);
}
int periodic_owner_begin(PeriodicParticipation *o, PeriodicOwnerInvocation *i) {
    if (!o || !i || o->status != PERIODIC_OWNER_READY || !versions_match(o)) return 0;
    const PeriodicOwnerAction *a = &o->program->actions[o->next_action];
    if (a->entry_phase != o->entry.phase || a->role != o->entry.role) return 0;
    for (size_t p = a->first_port; p < (size_t)a->first_port + a->port_count; ++p) {
        o->bindings[p].returned = 0;
        o->bindings[p].exit_version = o->bindings[p].entry_version;
    }
    o->closed = 0;
    o->status = PERIODIC_OWNER_OPEN;
    *i = (PeriodicOwnerInvocation){o, ++o->invocation, o->revision, o->entry}; return 1;
}
int periodic_owner_return(PeriodicParticipation *o, const PeriodicOwnerInvocation *i,
    size_t port, PeriodicDependency *d, uint64_t generation) {
    if (!owns(o, i) || port >= o->program->port_count) return 0;
    const PeriodicOwnerAction *a = &o->program->actions[o->next_action];
    if (port < a->first_port || port - a->first_port >= a->port_count) return 0;
    OwnerBinding *b = &o->bindings[port];
    if (b->dependency != d || !d || d->generation != generation || b->entry_version != generation) return 0;
    if (b->returned) return b->exit_version == generation;
    b->returned = 1; return 1;
}
int periodic_owner_write_return(PeriodicParticipation *o, const PeriodicOwnerInvocation *i,
    size_t port, PeriodicDependency *d, uint64_t entry, uint64_t exit) {
    if (!owns(o, i) || port >= o->program->port_count || !exit || exit < entry ||
        !(o->program->ports[port].access & PERIODIC_OWNER_WRITE)) return 0;
    const PeriodicOwnerAction *a = &o->program->actions[o->next_action];
    if (port < a->first_port || port - a->first_port >= a->port_count) return 0;
    OwnerBinding *b = &o->bindings[port];
    if (b->dependency != d || !d || d->generation != entry || b->entry_version != entry) return 0;
    // Repeating a receipt is idempotent; replacing an already returned result
    // would let two incompatible exits discharge the same obligation.
    if (b->returned) return b->exit_version == exit;
    b->exit_version = exit; b->returned = 1; return 1;
}
int periodic_owner_complete(PeriodicParticipation *o, const PeriodicOwnerInvocation *i) {
    if (!owns(o, i) || !versions_match(o)) return 0;
    const PeriodicOwnerAction *a = &o->program->actions[o->next_action];
    for (size_t p = a->first_port; p < (size_t)a->first_port + a->port_count; ++p)
        if (!o->bindings[p].returned) return 0;
    // Aliased write ports must agree before any output becomes visible. The
    // graph is owner-thread serialized; its visit stamp avoids a pairwise scan.
    uint64_t visit = ++o->program->graph->traversal;
    for (size_t p = a->first_port; p < (size_t)a->first_port + a->port_count; ++p) {
        if (!(o->program->ports[p].access & PERIODIC_OWNER_WRITE)) continue;
        OwnerBinding *b = &o->bindings[p];
        PeriodicDependency *d = b->dependency;
        if (d->validation_visit == visit && d->validation_version != b->exit_version) return 0;
        d->validation_visit = visit; d->validation_version = b->exit_version;
    }
    PeriodicParticipation *affected = NULL;
    for (size_t p = a->first_port; p < (size_t)a->first_port + a->port_count; ++p) {
        OwnerBinding *b = &o->bindings[p];
        if (!(o->program->ports[p].access & PERIODIC_OWNER_WRITE) ||
            b->exit_version == b->dependency->generation) continue;
        b->dependency->generation = b->exit_version;
        for (OwnerBinding *r = b->dependency->users; r; r = r->next) {
            if (r->owner == o) r->entry_version = b->exit_version;
            else enqueue(&affected, r->owner, visit);
        }
    }
    o->entry.phase = a->exit_phase;
    o->next_action = (o->next_action + 1) % o->program->action_count;
    o->entry.role = o->program->actions[o->next_action].role;
    o->status = PERIODIC_OWNER_READY;
    if (same_entry(o->entry, o->cycle_entry)) {
        int returned = 1;
        for (size_t p = 0; p < o->program->port_count; ++p)
            returned &= o->bindings[p].returned &&
                o->bindings[p].dependency->generation == o->bindings[p].cycle_version;
        o->closed = returned;
    }
    // A causal path through another writer can return to this participation.
    // Only the direct self-edge is omitted; a feedback contact still revises it.
    revise_cone(affected, visit);
    return 1;
}
void periodic_owner_abort(PeriodicParticipation *o, const PeriodicOwnerInvocation *i) {
    if (owns(o, i)) periodic_owner_contact(o);
}
int periodic_owner_closed(const PeriodicParticipation *o) {
    return o && o->closed && o->status == PERIODIC_OWNER_READY &&
        same_entry(o->entry, o->cycle_entry) && versions_match(o);
}
PeriodicOwnerState periodic_owner_state(const PeriodicParticipation *o) {
    if (!o) return (PeriodicOwnerState){0};
    return (PeriodicOwnerState){o->program->id, o->entry, o->revision, o->next_action,
        o->status, periodic_owner_closed(o)};
}

typedef struct {
    uint64_t magic, implementation, action_count, port_count, next_action;
    PeriodicCausalEntry entry, cycle_entry;
    uint64_t status, closed;
} Checkpoint;
typedef struct { uint64_t identity, generation, entry_version, cycle_version, exit_version, returned; } SavedBinding;
size_t periodic_owner_checkpoint_size(const PeriodicParticipation *o) {
    if (!o || o->program->action_count > (SIZE_MAX - sizeof(Checkpoint)) / sizeof(PeriodicOwnerAction)) return 0;
    size_t size = sizeof(Checkpoint) + o->program->action_count * sizeof(PeriodicOwnerAction);
    size_t stride = sizeof(PeriodicOwnerPort) + sizeof(SavedBinding);
    if (o->program->port_count > (SIZE_MAX - size) / stride) return 0;
    return size + o->program->port_count * stride;
}
int periodic_owner_checkpoint(const PeriodicParticipation *o, void *bytes, size_t size) {
    if (!o || !bytes || !size || o->status == PERIODIC_OWNER_OPEN || !bound(o) || size != periodic_owner_checkpoint_size(o)) return 0;
    Checkpoint h = {.magic=UINT64_C(0x504f574e45523032), .implementation=o->program->implementation,
        .action_count=o->program->action_count, .port_count=o->program->port_count,
        .next_action=o->next_action, .entry=o->entry, .cycle_entry=o->cycle_entry,
        .status=o->status, .closed=o->closed};
    unsigned char *p = bytes; memcpy(p, &h, sizeof h); p += sizeof h;
    memcpy(p, o->program->actions, h.action_count * sizeof(PeriodicOwnerAction)); p += h.action_count * sizeof(PeriodicOwnerAction);
    if (h.port_count) memcpy(p, o->program->ports, h.port_count * sizeof(PeriodicOwnerPort)); p += h.port_count * sizeof(PeriodicOwnerPort);
    for (size_t j = 0; j < h.port_count; ++j) {
        const OwnerBinding *b = &o->bindings[j];
        SavedBinding s = {b->dependency->identity, b->dependency->generation, b->entry_version,
            b->cycle_version, b->exit_version, b->returned};
        memcpy(p, &s, sizeof s); p += sizeof s;
    }
    return 1;
}
int periodic_owner_restore(PeriodicParticipation *o, const void *bytes, size_t size) {
    if (!o || !bytes || !size || o->status == PERIODIC_OWNER_OPEN || !bound(o) || size != periodic_owner_checkpoint_size(o)) return 0;
    Checkpoint h; const unsigned char *p = bytes; memcpy(&h, p, sizeof h); p += sizeof h;
    if (h.magic != UINT64_C(0x504f574e45523032) || h.implementation != o->program->implementation ||
        h.action_count != o->program->action_count || h.port_count != o->program->port_count ||
        h.next_action >= h.action_count || h.status == PERIODIC_OWNER_OPEN || h.status > PERIODIC_OWNER_REVISED ||
        h.entry.owner != o->entry.owner || h.entry.generation != o->entry.generation ||
        !h.entry.parameter_generation || h.entry.parameter_generation != o->entry.parameter_generation ||
        h.entry.phase != o->program->actions[h.next_action].entry_phase ||
        h.entry.role != o->program->actions[h.next_action].role || h.closed > 1) return 0;
    if (h.cycle_entry.owner != h.entry.owner || h.cycle_entry.generation != h.entry.generation ||
        h.cycle_entry.parameter_generation != h.entry.parameter_generation ||
        (h.status == PERIODIC_OWNER_REVISED && h.closed)) return 0;
    size_t cycle = 0;
    while (cycle < h.action_count && (h.cycle_entry.phase != o->program->actions[cycle].entry_phase ||
            h.cycle_entry.role != o->program->actions[cycle].role)) ++cycle;
    if (cycle == h.action_count) return 0;
    for (size_t j = 0; j < h.action_count; ++j) {
        PeriodicOwnerAction a; memcpy(&a, p, sizeof a); p += sizeof a;
        const PeriodicOwnerAction *e = &o->program->actions[j];
        if (a.entry_phase != e->entry_phase || a.exit_phase != e->exit_phase || a.role != e->role ||
            a.first_port != e->first_port || a.port_count != e->port_count) return 0;
    }
    for (size_t j = 0; j < h.port_count; ++j) {
        PeriodicOwnerPort a; memcpy(&a, p, sizeof a); p += sizeof a;
        if (a.role != o->program->ports[j].role || a.access != o->program->ports[j].access) return 0;
    }
    const unsigned char *bindings = p;
    for (size_t j = 0; j < h.port_count; ++j) {
        SavedBinding s; memcpy(&s, p, sizeof s); p += sizeof s;
        if (s.identity != o->bindings[j].dependency->identity || s.generation != o->bindings[j].dependency->generation ||
            s.returned > 1 || (h.status == PERIODIC_OWNER_READY && s.entry_version != s.generation) ||
            (h.closed && (!s.returned || s.cycle_version != s.generation))) return 0;
    }
    if (h.closed && !same_entry(h.entry, h.cycle_entry)) return 0;
    periodic_owner_contact(o); // Previous process-local receipts stay invalid.
    o->entry = h.entry; o->cycle_entry = h.cycle_entry; o->next_action = h.next_action;
    o->status = (PeriodicOwnerStatus)h.status; o->closed = (unsigned)h.closed;
    for (size_t j = 0; j < h.port_count; ++j) {
        SavedBinding s; memcpy(&s, bindings, sizeof s); bindings += sizeof s;
        o->bindings[j].entry_version = s.entry_version;
        o->bindings[j].cycle_version = s.cycle_version;
        o->bindings[j].exit_version = s.exit_version;
        o->bindings[j].returned = (unsigned)s.returned;
    }
    return 1;
}
