#include "harness.h"
#include "runner.h"

#include "util/json.h"

using namespace isolate;
using namespace isolate::test;

// The command-line surface: argument handling, help, and the behaviour a
// script would depend on.

TEST(cli, no_arguments_prints_usage) {
    Environment env;
    Run run = env.isolate({});
    CHECK(run.ok());
    CHECK(run.out_contains("usage: isolate"));
    CHECK(run.out_contains("run"));
    CHECK(run.out_contains("diff"));
}

TEST(cli, help_lists_the_commands) {
    Environment env;
    Run run = env.isolate({"--help"});
    CHECK(run.ok());
    for (const char* command : {"run", "enter", "ls", "inspect", "ps", "diff", "commit",
                                "snapshot", "kill", "destroy", "doctor"}) {
        CHECK(run.out_contains(command));
    }
}

TEST(cli, each_command_has_its_own_help) {
    Environment env;
    for (const char* command : {"run", "enter", "ls", "inspect", "ps", "diff", "commit",
                                "snapshot", "kill", "destroy", "doctor"}) {
        Run run = env.isolate({command, "--help"});
        CHECK(run.ok());
        CHECK(run.out_contains("isolate"));
        CHECK(run.out.size() > 60);
    }
}

TEST(cli, version_is_reported) {
    Environment env;
    Run run = env.isolate({"--version"});
    CHECK(run.ok());
    CHECK(run.out_contains("isolate"));
    CHECK(run.out_contains("0."));
}

TEST(cli, an_unknown_command_is_reported_with_a_hint) {
    Environment env;
    Run run = env.isolate({"frobnicate"});
    CHECK_EQ(run.status, 2);
    CHECK(run.err_contains("unknown command"));
    CHECK(run.err_contains("--help"));
}

TEST(cli, an_unknown_option_is_reported) {
    Environment env;
    Run run = env.isolate({"run", "--nonsense", "--", "/usr/bin/true"});
    CHECK(!run.ok());
    CHECK(run.err_contains("unknown option"));
}

TEST(cli, options_before_a_command_are_rejected) {
    Environment env;
    Run run = env.isolate({"--verbose", "ls"});
    CHECK(!run.ok());
    CHECK(run.err_contains("expected a command"));
}

TEST(cli, an_option_without_its_value_is_reported) {
    Environment env;
    Run run = env.isolate({"run", "--memory"});
    CHECK(!run.ok());
    CHECK(run.err_contains("needs a value"));
}

TEST(cli, options_accept_both_separated_and_joined_values) {
    Environment env;
    env.write("data/file", "x\n");

    Run separated = env.isolate({"run", "--ephemeral", "--ro", env.path("data"), "--",
                                 "/bin/cat", env.path("data/file")});
    CHECK(separated.ok());

    Run joined = env.isolate({"run", "--ephemeral", "--ro=" + env.path("data"), "--",
                              "/bin/cat", env.path("data/file")});
    CHECK(joined.ok());
}

TEST(cli, comma_separated_paths_are_accepted) {
    Environment env;
    env.write("a/file", "from-a\n");
    env.write("b/file", "from-b\n");

    Run run = env.run_shell({"--ro", env.path("a") + "," + env.path("b")},
                            "cat " + env.path("a/file") + "; cat " + env.path("b/file"));
    CHECK(run.ok());
    CHECK(run.out_contains("from-a"));
    CHECK(run.out_contains("from-b"));
}

TEST(cli, everything_after_the_program_belongs_to_the_program) {
    // A program's own --verbose must not be eaten by isolate.
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--", "/bin/echo", "--verbose", "--memory",
                           "1G", "--help"});
    CHECK(run.ok());
    CHECK(run.out_contains("--verbose --memory 1G --help"));
}

TEST(cli, a_program_can_be_given_without_the_separator) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "/bin/echo", "hello"});
    CHECK(run.ok());
    CHECK(run.out_contains("hello"));
}

TEST(cli, ls_on_an_empty_store_says_so) {
    Environment env;
    Run run = env.isolate({"ls"});
    CHECK(run.ok());
    CHECK(run.out_contains("no capsules"));
}

TEST(cli, ls_shows_a_table_with_the_expected_columns) {
    Environment env;
    Run created = env.isolate({"run", "--name", "listed", "--", "/usr/bin/true"});
    CHECK(created.ok());

    Run run = env.isolate({"ls"});
    CHECK(run.out_contains("ID"));
    CHECK(run.out_contains("NAME"));
    CHECK(run.out_contains("STATUS"));
    CHECK(run.out_contains("SIZE"));
    CHECK(run.out_contains("listed"));
    CHECK(run.out_contains("stopped"));
}

TEST(cli, ls_json_is_an_array) {
    Environment env;
    CHECK(env.isolate({"run", "--name", "one", "--", "/usr/bin/true"}).ok());
    CHECK(env.isolate({"run", "--name", "two", "--", "/usr/bin/true"}).ok());

    Run run = env.isolate({"ls", "--json"});
    CHECK(run.ok());
    json::Value parsed = json::parse(run.out);
    CHECK(parsed.is_array());
    CHECK_EQ(parsed.as_array().size(), size_t(2));
    for (const json::Value& capsule : parsed.as_array()) {
        CHECK(!capsule.at("id").as_string().empty());
        CHECK(!capsule.at("status").as_string().empty());
    }
}

TEST(cli, ls_json_on_an_empty_store_is_an_empty_array) {
    Environment env;
    Run run = env.isolate({"ls", "--json"});
    CHECK(run.ok());
    CHECK(json::parse(run.out).as_array().empty());
}

TEST(cli, inspect_shows_the_policy) {
    Environment env;
    env.write("project/file", "x\n");
    Run created = env.isolate({"run", "--name", "described", "--cow", env.path("project"),
                               "--ro", "/usr/local", "--network", "outbound", "--services",
                               "development", "--", "/usr/bin/true"});
    CHECK(created.ok());

    Run run = env.isolate({"inspect", "described"});
    CHECK(run.ok());
    CHECK(run.out_contains("filesystem"));
    CHECK(run.out_contains("cow"));
    CHECK(run.out_contains(env.path("project")));
    CHECK(run.out_contains("outbound"));
    CHECK(run.out_contains("development"));
    CHECK(run.out_contains("private tmp"));
    CHECK(run.out_contains("credentials"));
}

TEST(cli, inspect_explains_a_capsule_with_no_mounts) {
    Environment env;
    Run created = env.isolate({"run", "--name", "bare", "--", "/usr/bin/true"});
    CHECK(created.ok());
    Run run = env.isolate({"inspect", "bare"});
    CHECK(run.out_contains("nothing mounted"));
}

TEST(cli, inspect_can_print_the_sandbox_profile) {
    Environment env;
    Run created = env.isolate({"run", "--name", "shown", "--", "/usr/bin/true"});
    CHECK(created.ok());

    Run run = env.isolate({"inspect", "--profile", "shown"});
    CHECK(run.ok());
    CHECK(run.out_contains("(version 1)"));
    CHECK(run.out_contains("(deny default"));
    CHECK(run.out_contains("Generated by isolate"));
    // The profile is commented, so a reader can see why each rule is there.
    CHECK(run.out_contains(";; Mach services"));
}

TEST(cli, inspect_json_includes_the_policy_and_the_allowed_services) {
    Environment env;
    Run created = env.isolate({"run", "--name", "described", "--", "/usr/bin/true"});
    CHECK(created.ok());

    Run run = env.isolate({"inspect", "--json", "described"});
    CHECK(run.ok());
    json::Value parsed = json::parse(run.out);
    CHECK(parsed.get("policy") != nullptr);
    CHECK(parsed.get("allowed_services") != nullptr);
    CHECK(!parsed.at("allowed_services").as_array().empty());
    CHECK(parsed.get("limit_enforcement") != nullptr);
    CHECK(parsed.get("snapshots") != nullptr);
}

TEST(cli, commands_report_a_capsule_that_does_not_exist) {
    Environment env;
    for (const char* command : {"inspect", "ps", "diff", "commit", "kill"}) {
        Run run = env.isolate({command, "nosuch"});
        CHECK(!run.ok());
        CHECK(run.err_contains("no capsule"));
    }
}

TEST(cli, commands_that_need_a_capsule_say_so_when_given_none) {
    Environment env;
    for (const char* command : {"inspect", "ps", "diff", "commit"}) {
        Run run = env.isolate({command});
        CHECK(!run.ok());
        CHECK(run.err_contains("expected"));
    }
}

TEST(cli, a_malformed_capsule_reference_is_rejected) {
    Environment env;
    Run run = env.isolate({"inspect", "../../etc/passwd"});
    CHECK(!run.ok());
    CHECK(run.err_contains("not a valid capsule name"));
}

TEST(cli, an_invalid_capsule_name_is_rejected) {
    Environment env;
    Run spaces = env.isolate({"run", "--name", "has space", "--", "/usr/bin/true"});
    CHECK(!spaces.ok());
    CHECK(spaces.err_contains("not a valid capsule name"));

    Run dots = env.isolate({"run", "--name", "../escape", "--", "/usr/bin/true"});
    CHECK(!dots.ok());
}

TEST(cli, quiet_suppresses_diagnostics) {
    Environment env;
    env.write("project/file", "x\n");
    Run run = env.isolate({"run", "--quiet", "--name", "quiet", "--cow",
                           env.path("project"), "--", "/usr/bin/true"});
    CHECK(run.ok());
    CHECK_EQ(run.err, std::string(""));
}

TEST(cli, verbose_describes_the_capsule_being_created) {
    Environment env;
    env.write("project/file", "x\n");
    Run run = env.isolate({"run", "--verbose", "--ephemeral", "--cow", env.path("project"),
                           "--", "/usr/bin/true"});
    CHECK(run.ok());
    CHECK(run.err_contains("capsule"));
    CHECK(run.err_contains("network: none"));
    CHECK(run.err_contains(env.path("project")));
}

TEST(cli, environment_variables_can_be_set_and_passed_through) {
    Environment env;
    Run set = env.run_shell({"--env", "GREETING=hello-from-policy"}, "echo $GREETING");
    CHECK(set.out_contains("hello-from-policy"));

    // A capsule starts with no environment unless told otherwise, which is why
    // the default here is empty.
    Run absent = env.run_shell({}, "echo [${PATH_TO_NOWHERE:-unset}]");
    CHECK(absent.out_contains("[unset]"));
}

TEST(cli, the_capsule_identity_is_in_the_environment) {
    Environment env;
    Run run = env.run_shell({}, "echo capsule=$ISOLATE_CAPSULE");
    CHECK(run.ok());
    CHECK(run.out_contains("capsule="));
    CHECK(run.out.size() > std::string("capsule=\n").size());
}

TEST(cli, a_named_capsule_exposes_its_name_too) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--name", "friendly", "--", "/bin/sh",
                           "-c", "echo name=$ISOLATE_CAPSULE_NAME"});
    CHECK(run.out_contains("name=friendly"));
}

TEST(cli, inherit_env_passes_the_callers_environment) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--inherit-env", "--", "/bin/sh", "-c",
                           "echo path=$PATH"});
    CHECK(run.ok());
    CHECK(run.out_contains("path=/usr/bin"));
}

TEST(cli, the_working_directory_can_be_set) {
    Environment env;
    env.write("elsewhere/marker", "here\n");
    Run run = env.isolate({"run", "--ephemeral", "--ro", env.path("elsewhere"), "--cwd",
                           env.path("elsewhere"), "--", "/bin/cat", "marker"});
    CHECK(run.ok());
    CHECK(run.out_contains("here"));
}

TEST(cli, a_service_name_can_be_added_to_the_policy) {
    Environment env;
    Run created = env.isolate({"run", "--name", "extra", "--service",
                               "com.apple.example.service", "--", "/usr/bin/true"});
    CHECK(created.ok());

    Run shown = env.isolate({"inspect", "--profile", "extra"});
    CHECK(shown.out_contains("com.apple.example.service"));
}

TEST(cli, the_service_bundle_can_be_closed_entirely) {
    Environment env;
    Run created = env.isolate({"run", "--name", "closed", "--services", "none", "--",
                               "/usr/bin/true"});
    CHECK(created.ok());

    Run shown = env.isolate({"inspect", "--profile", "closed"});
    CHECK(!shown.out_contains("com.apple.logd"));
    CHECK(shown.out_contains("no service lookups are permitted"));
}

TEST(cli, count_options_reject_non_numeric_values) {
    // atoi would turn "abc" into 0 and the error would then describe a limit
    // the user never asked for.
    Environment env;
    Run processes = env.isolate({"run", "--ephemeral", "--processes", "abc", "--",
                                 "/usr/bin/true"});
    CHECK(!processes.ok());
    CHECK(processes.err_contains("positive whole number"));

    Run files = env.isolate({"run", "--ephemeral", "--max-open-files", "many", "--",
                             "/usr/bin/true"});
    CHECK(!files.ok());
    CHECK(files.err_contains("positive whole number"));

    Run negative = env.isolate({"run", "--ephemeral", "--processes", "-5", "--",
                                "/usr/bin/true"});
    CHECK(!negative.ok());
}

TEST(cli, umask_must_be_octal) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--umask", "999", "--", "/usr/bin/true"});
    CHECK(!run.ok());
    CHECK(run.err_contains("octal"));

    Run valid = env.isolate({"run", "--ephemeral", "--umask", "027", "--", "/bin/sh", "-c",
                             "/usr/bin/touch $TMPDIR/f && /bin/ls -l $TMPDIR/f"});
    CHECK(valid.ok());
    CHECK(valid.out_contains("rw-r-----"));
}

TEST(cli, an_unknown_service_bundle_is_rejected) {
    Environment env;
    Run run = env.isolate({"run", "--ephemeral", "--services", "everything", "--",
                           "/usr/bin/true"});
    CHECK(!run.ok());
    CHECK(run.err_contains("expected none, minimal, development, or graphical"));
}

TEST(cli, doctor_runs_without_a_capsule) {
    Environment env;
    Run run = env.isolate({"doctor"});
    CHECK(run.ok());
    CHECK(run.out_contains("capsule store"));
}

TEST(cli, the_store_location_follows_the_environment) {
    Environment env;
    Run run = env.isolate({"doctor"});
    CHECK(run.out_contains(env.store()));
}
