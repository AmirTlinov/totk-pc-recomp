#include "periodic_owner.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

static PeriodicOwnerInvocation begin(PeriodicParticipation *p) {
    PeriodicOwnerInvocation invocation;
    assert(periodic_owner_begin(p, &invocation));
    return invocation;
}
static void complete(PeriodicParticipation *p, PeriodicDependency *read, uint64_t version, PeriodicDependency *write) {
    PeriodicOwnerInvocation i = begin(p);
    if (read) {
        assert(!periodic_owner_complete(p, &i));
        assert(periodic_owner_return(p, &i, 0, read, version));
        assert(!periodic_owner_complete(p, &i));
        assert(periodic_owner_return(p, &i, 1, write, 1));
    }
    assert(periodic_owner_complete(p, &i));
    assert(!periodic_owner_complete(p, &i));
}
static void produced_versions(void) {
    PeriodicOwnerGraph *g = periodic_owner_graph_create(); assert(g);
    const PeriodicOwnerAction step = {0, 0, 1, 0, 2};
    const PeriodicOwnerPort ports[] = {{1, PERIODIC_OWNER_READ}, {2, PERIODIC_OWNER_WRITE}};
    const PeriodicOwnerProgram *program = periodic_owner_program(g, 999, &step, 1, ports, 2);
    PeriodicDependency *input = periodic_owner_dependency(g, 1, 1);
    PeriodicDependency *image = periodic_owner_dependency(g, 2, 1);
    PeriodicDependency *output = periodic_owner_dependency(g, 3, 1);
    PeriodicParticipation *writer = periodic_owner_create(g, program, (PeriodicCausalEntry){1,1,1,0,1});
    PeriodicParticipation *reader = periodic_owner_create(g, program, (PeriodicCausalEntry){2,1,1,0,1});
    assert(periodic_owner_bind(writer, 0, input)); assert(periodic_owner_bind(writer, 1, image));
    assert(periodic_owner_bind(reader, 0, image)); assert(periodic_owner_bind(reader, 1, output));
    assert(periodic_owner_revalidate(writer, 1)); assert(periodic_owner_revalidate(reader, 1));
    PeriodicOwnerInvocation w = begin(writer), r = begin(reader);
    assert(!periodic_owner_write_return(writer, &w, 0, input, 1, 2));
    assert(!periodic_owner_write_return(reader, &w, 1, output, 1, 2));
    assert(periodic_owner_write_return(writer, &w, 1, image, 1, 2));
    assert(!periodic_owner_write_return(writer, &w, 1, image, 1, 3));
    assert(!periodic_owner_return(writer, &w, 1, image, 1));
    assert(!periodic_owner_complete(writer, &w));
    // Staging an output does not expose it before all input returns arrive.
    assert(periodic_owner_return(reader, &r, 0, image, 1));
    assert(periodic_owner_return(writer, &w, 0, input, 1));
    assert(periodic_owner_complete(writer, &w));
    assert(periodic_owner_state(writer).status == PERIODIC_OWNER_READY);
    assert(!periodic_owner_closed(writer));
    assert(periodic_owner_state(reader).status == PERIODIC_OWNER_REVISED);
    assert(!periodic_owner_complete(reader, &r));
    // The same program executes another output generation without recompilation
    // or a self-contact, and checkpointing must not turn it into exact closure.
    size_t size = periodic_owner_checkpoint_size(writer); void *saved = malloc(size); assert(saved);
    assert(periodic_owner_checkpoint(writer, saved, size));
    assert(periodic_owner_restore(writer, saved, size)); assert(!periodic_owner_closed(writer));
    free(saved);
    w = begin(writer);
    assert(periodic_owner_return(writer, &w, 0, input, 1));
    assert(periodic_owner_write_return(writer, &w, 1, image, 2, 3));
    periodic_owner_abort(writer, &w);
    assert(periodic_owner_dependency(g, 2, 2) == image);
    assert(periodic_owner_revalidate(writer, 1));
    w = begin(writer);
    assert(periodic_owner_return(writer, &w, 0, input, 1));
    assert(periodic_owner_write_return(writer, &w, 1, image, 2, 3));
    assert(periodic_owner_complete(writer, &w));
    assert(periodic_owner_dependency(g, 2, 3) == image);
    assert(!periodic_owner_closed(writer));
    const PeriodicOwnerPort aliases[] = {{1, PERIODIC_OWNER_WRITE}, {2, PERIODIC_OWNER_WRITE}};
    const PeriodicOwnerProgram *aliased = periodic_owner_program(g, 998, &step, 1, aliases, 2);
    PeriodicParticipation *both = periodic_owner_create(g, aliased, (PeriodicCausalEntry){3,1,1,0,1});
    assert(periodic_owner_bind(both, 0, output)); assert(periodic_owner_bind(both, 1, output));
    assert(periodic_owner_revalidate(both, 1));
    PeriodicOwnerInvocation pair = begin(both);
    assert(periodic_owner_write_return(both, &pair, 0, output, 1, 2));
    assert(periodic_owner_write_return(both, &pair, 1, output, 1, 3));
    assert(!periodic_owner_complete(both, &pair));
    assert(periodic_owner_dependency(g, 3, 1) == output);
    periodic_owner_abort(both, &pair);
    periodic_owner_graph_destroy(g);
}
int main(void) {
    produced_versions();
    PeriodicOwnerGraph *g = periodic_owner_graph_create(); assert(g);
    const PeriodicOwnerAction cycle[] = {{10, 20, 7, 0, 2}, {20, 30, 7, 0, 2}, {30, 10, 7, 0, 2}};
    const PeriodicOwnerPort ports[] = {{41, PERIODIC_OWNER_READ}, {42, PERIODIC_OWNER_WRITE}};
    const PeriodicOwnerProgram *p = periodic_owner_program(g, 100, cycle, 3, ports, 2); assert(p);
    assert(periodic_owner_program(g, 100, cycle, 3, ports, 2) == p);
    PeriodicParticipation *a = periodic_owner_create(g, p, (PeriodicCausalEntry){1, 1, 7, 10, 1});
    PeriodicParticipation *b = periodic_owner_create(g, p, (PeriodicCausalEntry){2, 1, 7, 10, 1});
    PeriodicParticipation *c = periodic_owner_create(g, p, (PeriodicCausalEntry){3, 1, 7, 10, 1});
    assert(a && b && c);
    assert(!periodic_owner_create(g, p, (PeriodicCausalEntry){1, 1, 7, 10, 1}));
    PeriodicDependency *input = periodic_owner_dependency(g, 11, 1);
    PeriodicDependency *ab = periodic_owner_dependency(g, 12, 1);
    PeriodicDependency *output = periodic_owner_dependency(g, 13, 1);
    PeriodicDependency *independent = periodic_owner_dependency(g, 14, 1);
    PeriodicDependency *other_output = periodic_owner_dependency(g, 15, 1);
    assert(periodic_owner_bind(a, 0, input) && periodic_owner_bind(a, 1, ab));
    assert(periodic_owner_bind(b, 0, ab) && periodic_owner_bind(b, 1, output));
    assert(periodic_owner_bind(c, 0, independent) && periodic_owner_bind(c, 1, other_output));
    assert(periodic_owner_revalidate(a, 1) && periodic_owner_revalidate(b, 1) && periodic_owner_revalidate(c, 1));
    // An unrelated owner may run in any order or submission. Only its own
    // returned action changes the local causal phase.
    complete(c, independent, 1, other_output); complete(a, input, 1, ab);
    assert(periodic_owner_state(a).entry.phase == 20);
    assert(!periodic_owner_create(g, p, (PeriodicCausalEntry){1, 1, 7, 10, 1}));
    const PeriodicOwnerAction routed[] = {{10, 20, 7, 0, 0}, {20, 10, 8, 0, 0}};
    assert(!periodic_owner_program(g, 300, routed, 2, NULL, 0));
    assert(periodic_owner_state(b).entry.phase == 10);
    complete(b, ab, 1, output); complete(a, input, 1, ab);
    assert(periodic_owner_state(a).entry.phase == 30);
    assert(periodic_owner_state(c).entry.phase == 20);
    // Exact return requires all addressed dependency contributions.
    PeriodicOwnerInvocation ai = begin(a), bi = begin(b);
    assert(!periodic_owner_return(b, &ai, 0, ab, 1));
    assert(!periodic_owner_complete(b, &ai));
    assert(!periodic_owner_return(a, &ai, 0, independent, 1));
    assert(periodic_owner_return(a, &ai, 0, input, 1));
    assert(periodic_owner_return(a, &ai, 1, ab, 1));
    assert(periodic_owner_complete(a, &ai));
    assert(periodic_owner_closed(a));
    assert(!periodic_owner_complete(b, &bi));
    assert(periodic_owner_return(b, &bi, 0, ab, 1));
    assert(periodic_owner_return(b, &bi, 1, output, 1));
    assert(periodic_owner_complete(b, &bi));
    assert(!periodic_owner_closed(b));
    // A cycle with missing new returns cannot reuse an old receipt.
    ai = begin(a); assert(!periodic_owner_complete(a, &ai));
    assert(!periodic_owner_closed(a));
    assert(periodic_owner_return(a, &ai, 0, input, 1));
    assert(periodic_owner_return(a, &ai, 1, ab, 1));
    assert(periodic_owner_complete(a, &ai));
    size_t partial_size = periodic_owner_checkpoint_size(a);
    void *partial = malloc(partial_size); assert(partial);
    assert(periodic_owner_checkpoint(a, partial, partial_size));
    assert(periodic_owner_restore(a, partial, partial_size));
    assert(periodic_owner_state(a).entry.phase == 20);
    free(partial);
    complete(a, input, 1, ab); complete(a, input, 1, ab);
    // Invalidating input affects A -> B and leaves C's phase/revision alone.
    PeriodicOwnerState cs = periodic_owner_state(c);
    ai = begin(a);
    periodic_owner_dependency_change(input, 2);
    assert(!periodic_owner_complete(a, &ai));
    assert(periodic_owner_state(a).status == PERIODIC_OWNER_REVISED);
    assert(periodic_owner_state(b).status == PERIODIC_OWNER_REVISED);
    assert(periodic_owner_state(c).revision == cs.revision);
    assert(periodic_owner_state(c).entry.phase == cs.entry.phase);
    assert(periodic_owner_revalidate(a, 2));
    assert(periodic_owner_state(a).program_id == periodic_owner_program_id(p));
    assert(periodic_owner_state(a).entry.phase == 10);
    // Equal-valued external contact still revokes an open contribution.
    ai = begin(a); periodic_owner_contact(a);
    assert(!periodic_owner_complete(a, &ai));
    assert(periodic_owner_revalidate(a, 2));
    complete(a, input, 2, ab);
    // Save a partial course. Restore keeps its local phase and invalidates
    // process-local receipts. Independent owners cannot load each other's state.
    size_t n = periodic_owner_checkpoint_size(a); void *saved = malloc(n); assert(saved);
    assert(periodic_owner_checkpoint(a, saved, n));
    complete(a, input, 2, ab); assert(periodic_owner_state(a).entry.phase == 30);
    assert(periodic_owner_restore(a, saved, n));
    assert(periodic_owner_state(a).entry.phase == 20);
    assert(!periodic_owner_restore(b, saved, n));
    periodic_owner_contact(a); assert(periodic_owner_revalidate(a, 3));
    assert(!periodic_owner_restore(a, saved, n));
    // Restoring external parameters explicitly is a separate operation.
    assert(periodic_owner_revalidate(a, 2));
    assert(periodic_owner_restore(a, saved, n));
    ai = begin(a); assert(!periodic_owner_checkpoint(a, saved, n));
    periodic_owner_abort(a, &ai); assert(!periodic_owner_complete(a, &ai));
    periodic_owner_dependency_change(input, 3);
    assert(!periodic_owner_restore(a, saved, n));
    free(saved);
    // No fixed owner/transition table or collision eviction: all live owners
    // survive beyond the old limits and share one immutable program.
    const PeriodicOwnerAction one = {0, 0, 1, 0, 0};
    const PeriodicOwnerProgram *empty = periodic_owner_program(g, 200, &one, 1, NULL, 0);
    for (uint64_t id = 100; id < 6000; ++id) {
        PeriodicParticipation *o = periodic_owner_create(g, empty, (PeriodicCausalEntry){id, 1, 1, 0, 1});
        assert(o); complete(o, NULL, 0, NULL); assert(periodic_owner_closed(o));
    }
    // Unrelated work never changes the original phase.
    assert(periodic_owner_state(c).entry.phase == cs.entry.phase);
    assert(!periodic_owner_dependency_destroy(independent));
    periodic_owner_destroy(c);
    assert(periodic_owner_dependency_destroy(independent));
    assert(periodic_owner_dependency_destroy(other_output));
    periodic_owner_graph_destroy(g);
    puts("periodic owners: independent phases, causal returns, dependency cones and checkpoints pass");
    return 0;
}
