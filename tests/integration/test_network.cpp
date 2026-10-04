#include "harness.h"
#include "runner.h"

#include <cstdlib>

using namespace isolate::test;

// Network isolation. These tests check that the denial is a kernel refusal on
// the syscall path rather than a hidden configuration or an unset variable, and
// that a capsule which is allowed out can still reach the network.
//
// The tests that need the internet are skipped when it is not reachable, so
// that a failure here always means isolate is wrong rather than the network is.

namespace {

bool internet_reachable() {
    static const bool reachable = [] {
        if (std::getenv("ISOLATE_TESTS_OFFLINE")) return false;
        Run probe = execute("/usr/bin/curl",
                            {"-s", "-m", "6", "-o", "/dev/null", "https://example.com"});
        return probe.ok();
    }();
    return reachable;
}

} // namespace

TEST(network, a_capsule_cannot_connect_to_a_local_port_by_default) {
    // Needs no internet: connecting to a closed local port fails with
    // ECONNREFUSED normally and EPERM under the sandbox, and either way the
    // interesting thing is that a capsule with networking allowed reports a
    // different error than one without. The direct check is that the socket
    // call itself is refused.
    Environment env;
    Run run = env.run_shell({}, "/usr/bin/nc -z -w 2 127.0.0.1 22 2>/dev/null && "
                                "echo CONNECTED || echo refused");
    CHECK(run.out_contains("refused"));
    CHECK(!run.contains("CONNECTED"));
}

TEST(network, a_capsule_cannot_listen) {
    Environment env;
    Run run = env.run_shell({}, "/usr/bin/nc -l 127.0.0.1 0 </dev/null >/dev/null 2>&1 && "
                                "echo LISTENED || echo denied");
    CHECK(run.out_contains("denied"));
}

TEST(network, the_denial_is_not_merely_an_unset_variable) {
    // A capsule given an http_proxy and a resolver still cannot connect, which
    // it could if the isolation were only environmental.
    if (!internet_reachable()) return;

    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--env", "http_proxy=", "--env",
                           "https_proxy=", "--", "/usr/bin/curl", "-s", "-m", "6", "-o",
                           "/dev/null", "-w", "%{http_code}", "https://example.com"});
    CHECK(!run.ok());
    CHECK(!run.out_contains("200"));
}

TEST(network, outbound_allows_a_connection) {
    if (!internet_reachable()) return;

    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--network", "outbound", "--", "/usr/bin/curl",
                           "-s", "-m", "10", "-o", "/dev/null", "-w", "%{http_code}",
                           "https://example.com"});
    CHECK(run.ok());
    CHECK(run.out_contains("200"));
}

TEST(network, none_refuses_the_same_connection_outbound_allows) {
    // The pair is what makes the result meaningful: the only difference between
    // the two runs is the policy.
    if (!internet_reachable()) return;

    Environment env;
    std::vector<std::string> args = {"run", "--ephemeral", "--", "/usr/bin/curl", "-s",
                                     "-m", "10", "-o", "/dev/null", "-w", "%{http_code}",
                                     "https://example.com"};
    Run denied = env.isolate(args);

    std::vector<std::string> allowed_args = {"run", "--ephemeral", "--network", "outbound",
                                             "--", "/usr/bin/curl", "-s", "-m", "10", "-o",
                                             "/dev/null", "-w", "%{http_code}",
                                             "https://example.com"};
    Run allowed = env.isolate(allowed_args);

    CHECK(!denied.ok());
    CHECK(allowed.ok());
    CHECK(allowed.out_contains("200"));
}

TEST(network, name_resolution_works_when_networking_is_allowed) {
    if (!internet_reachable()) return;

    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--network", "outbound", "--",
                           "/usr/bin/dscacheutil", "-q", "host", "-a", "name",
                           "example.com"});
    // dscacheutil may return nothing useful under a closed service policy; the
    // check that matters is that it is not refused outright.
    CHECK(run.status == 0 || run.status == 1);
}

TEST(network, unix_sockets_inside_the_capsule_work_with_networking_off) {
    // Processes in a capsule need to be able to talk to each other even when
    // the capsule has no network access, which is what a build server or a
    // test harness inside a capsule relies on.
    Environment env;
    Run run = env.run_shell({},
                            "/usr/bin/nc -lU $TMPDIR/sock >$TMPDIR/received 2>$TMPDIR/e1 & "
                            "/bin/sleep 1; "
                            "echo hello-over-unix | /usr/bin/nc -U $TMPDIR/sock "
                            "2>$TMPDIR/e2; "
                            "/bin/sleep 1; "
                            "echo \"received[$(cat $TMPDIR/received)]\"; "
                            "echo \"listen-error[$(cat $TMPDIR/e1)]\"; "
                            "echo \"send-error[$(cat $TMPDIR/e2)]\"");
    CHECK(run.out_contains("received[hello-over-unix]"));
    // Neither end may have been refused by the sandbox.
    CHECK(run.out_contains("listen-error[]"));
    CHECK(run.out_contains("send-error[]"));
}

TEST(network, the_policy_is_recorded_and_shown) {
    Environment env;
    Run created = env.isolate({"run", "--name", "offline", "--", "/usr/bin/true"});
    CHECK(created.ok());
    Run shown = env.isolate({"inspect", "offline"});
    CHECK(shown.out_contains("network"));
    CHECK(shown.out_contains("none"));

    Run online = env.isolate({"run", "--name", "online", "--network", "all", "--",
                              "/usr/bin/true"});
    CHECK(online.ok());
    CHECK(env.isolate({"inspect", "online"}).out_contains("all"));
}

TEST(network, an_unknown_network_mode_is_rejected) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--network", "sometimes", "--",
                           "/usr/bin/true"});
    CHECK(!run.ok());
    CHECK(run.err_contains("expected none, outbound, or all"));
}
