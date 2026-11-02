// Addressed continuations. The caller serializes each graph on its owner thread.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct PeriodicOwnerGraph PeriodicOwnerGraph;
typedef struct PeriodicOwnerProgram PeriodicOwnerProgram;
typedef struct PeriodicParticipation PeriodicParticipation;
typedef struct PeriodicDependency PeriodicDependency;

typedef struct {
    uint64_t entry_phase, exit_phase;
    uint32_t role;
    // This action must receive these addressed returns before it can advance.
    uint32_t first_port, port_count;
} PeriodicOwnerAction;

typedef struct {
    uint32_t role;
    uint32_t access;
} PeriodicOwnerPort;
enum { PERIODIC_OWNER_READ = 1, PERIODIC_OWNER_WRITE = 2 };

typedef struct {
    uint64_t owner, generation, role, phase, parameter_generation;
} PeriodicCausalEntry;

// A receipt addresses one open contribution. Equal results from another
// participation or an earlier invocation cannot discharge this contribution.
typedef struct {
    const PeriodicParticipation *participation;
    uint64_t invocation, revision;
    PeriodicCausalEntry entry;
} PeriodicOwnerInvocation;

typedef enum {
    PERIODIC_OWNER_READY, PERIODIC_OWNER_OPEN, PERIODIC_OWNER_REVISED
} PeriodicOwnerStatus;

typedef struct {
    uint64_t program_id;
    PeriodicCausalEntry entry;
    uint64_t revision;
    size_t next_action;
    PeriodicOwnerStatus status;
    int closed;
} PeriodicOwnerState;

// Creation, interning, binding and destruction are control-path operations.
// Program identity contains topology and port roles, never dynamic values.
// An action route keeps one participation role; role-changing routes require
// an explicit correspondence and are rejected by this renderer model.
PeriodicOwnerGraph *periodic_owner_graph_create(void);
void periodic_owner_graph_destroy(PeriodicOwnerGraph *graph);
const PeriodicOwnerProgram *periodic_owner_program(PeriodicOwnerGraph *graph,
    uint64_t implementation, const PeriodicOwnerAction *actions, size_t action_count,
    const PeriodicOwnerPort *ports, size_t port_count);
uint64_t periodic_owner_program_id(const PeriodicOwnerProgram *program);
PeriodicParticipation *periodic_owner_create(PeriodicOwnerGraph *graph,
    const PeriodicOwnerProgram *program, PeriodicCausalEntry entry);
void periodic_owner_destroy(PeriodicParticipation *owner);
PeriodicDependency *periodic_owner_dependency(PeriodicOwnerGraph *graph,
    uint64_t identity, uint64_t generation);
// Retire an external resource only after all bindings have released it.
int periodic_owner_dependency_destroy(PeriodicDependency *dependency);
int periodic_owner_bind(PeriodicParticipation *owner, size_t port,
    PeriodicDependency *dependency);

// These operations allocate nothing and do not scan unrelated owners.
// A write/contact revises the reverse dependency cone, including open receipts.
void periodic_owner_dependency_change(PeriodicDependency *dependency, uint64_t generation);
void periodic_owner_contact(PeriodicParticipation *owner);
// Explicitly accept the current bindings and parameters after a contact.
// The local phase is preserved, including a partial traversal of the program.
int periodic_owner_revalidate(PeriodicParticipation *owner, uint64_t parameter_generation);
int periodic_owner_begin(PeriodicParticipation *owner, PeriodicOwnerInvocation *invocation);
int periodic_owner_return(PeriodicParticipation *owner, const PeriodicOwnerInvocation *invocation,
    size_t port, PeriodicDependency *dependency, uint64_t generation);
// Stage this action's write. The dependency remains at its entry version until
// complete atomically publishes every output. A successful action may produce
// new versions without claiming a return to its exact causal entry.
int periodic_owner_write_return(PeriodicParticipation *owner,
    const PeriodicOwnerInvocation *invocation, size_t port,
    PeriodicDependency *dependency, uint64_t entry_generation, uint64_t exit_generation);
int periodic_owner_complete(PeriodicParticipation *owner, const PeriodicOwnerInvocation *invocation);
void periodic_owner_abort(PeriodicParticipation *owner, const PeriodicOwnerInvocation *invocation);
int periodic_owner_closed(const PeriodicParticipation *owner);
PeriodicOwnerState periodic_owner_state(const PeriodicParticipation *owner);

// Checkpoints contain no raw pointers. Restore validates the program, owner,
// local phase and every binding. The caller must first restore the matching
// parameter data/token; this snapshot cannot restore external data by itself.
// An in-flight contribution cannot be saved.
size_t periodic_owner_checkpoint_size(const PeriodicParticipation *owner);
int periodic_owner_checkpoint(const PeriodicParticipation *owner, void *bytes, size_t size);
int periodic_owner_restore(PeriodicParticipation *owner, const void *bytes, size_t size);
