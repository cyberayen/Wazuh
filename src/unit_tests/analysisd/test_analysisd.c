/*
 * Copyright (C) 2015, Wazuh Inc.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>

#include "../../analysisd/analysisd.h"

extern sem_t credits_eps_semaphore;
//extern pthread_mutex_t limit_eps_mutex = PTHREAD_MUTEX_INITIALIZER;
//extern pthread_mutex_t wait_sem = PTHREAD_MUTEX_INITIALIZER;
extern limits_t limits;
extern unsigned int limits_wait_counter;

void generate_eps_credits(unsigned int credits);
void clean_eps_credits(unsigned int credits);
void increase_event_counter(void);
void limits_free(void);
void inc_wait_counter(void);
void dec_wait_counter(void);
void get_eps_credit(void);
void load_limits(void);

// Setup / Teardown
static int test_setup(void **state) {
    memset(&limits, 0, sizeof(limits));
    limits.timeframe = 10;
    limits.eps = 10;
    limits.max_eps = limits.eps * limits.timeframe;
    limits.enabled = true;
    os_calloc(limits.timeframe, sizeof(unsigned int), limits.circ_buf);

    return OS_SUCCESS;
}

static int test_teardown(void **state) {
    if (limits.circ_buf) {
        os_free(limits.circ_buf);
    }
    return OS_SUCCESS;
}

/* Tests */
// generate_eps_credits
void test_generate_eps_credits_ok(void ** state)
{
    int current_credits;
    sem_init(&credits_eps_semaphore, 0, 0);

    generate_eps_credits(5);

    sem_getvalue(&credits_eps_semaphore, &current_credits);
    assert_int_equal(5, current_credits);

    sem_destroy(&credits_eps_semaphore);
}

void test_generate_eps_credits_ok_zero(void ** state)
{
    int current_credits;
    sem_init(&credits_eps_semaphore, 0, 0);

    generate_eps_credits(0);

    sem_getvalue(&credits_eps_semaphore, &current_credits);
    assert_int_equal(0, current_credits);

    sem_destroy(&credits_eps_semaphore);
}

// clean_eps_credits
void test_clean_eps_credits_ok(void ** state)
{
    int current_credits;
    sem_init(&credits_eps_semaphore, 0, 5);

    clean_eps_credits(5);

    sem_getvalue(&credits_eps_semaphore, &current_credits);
    assert_int_equal(0, current_credits);

    sem_destroy(&credits_eps_semaphore);
}

void test_clean_eps_credits_ok_3(void ** state)
{
    int current_credits;
    sem_init(&credits_eps_semaphore, 0, 5);

    clean_eps_credits(3);

    sem_getvalue(&credits_eps_semaphore, &current_credits);
    assert_int_equal(2, current_credits);

    sem_destroy(&credits_eps_semaphore);
}

// increase_event_counter
void test_increase_event_counter_ok(void ** state)
{
    assert_int_equal(0, limits.circ_buf[limits.current_cell]);
    increase_event_counter();
    assert_int_equal(1, limits.circ_buf[limits.current_cell]);
}

// inc_wait_counter
void test_inc_wait_counter_ok(void ** state)
{
    limits_wait_counter = 0;
    assert_int_equal(0, limits_wait_counter);
    inc_wait_counter();
    assert_int_equal(1, limits_wait_counter);
}

// dec_wait_counter
void test_dec_wait_counter_ok(void ** state)
{
    limits_wait_counter = 1;
    assert_int_equal(1, limits_wait_counter);
    dec_wait_counter();
    assert_int_equal(0, limits_wait_counter);
}

// get_eps_credit
void test_get_eps_credit_ok(void ** state)
{
    int current_credits;
    sem_init(&credits_eps_semaphore, 0, 5);
    limits_wait_counter = 0;

    get_eps_credit();
    assert_int_equal(0, limits_wait_counter);

    sem_getvalue(&credits_eps_semaphore, &current_credits);
    assert_int_equal(4, current_credits);
    assert_int_equal(1, limits.circ_buf[limits.current_cell]);

    sem_destroy(&credits_eps_semaphore);
}

// limits_free
void test_limits_free_ok(void ** state)
{
    int current_credits;
    sem_init(&credits_eps_semaphore, 0, 5);
    limits_wait_counter = 1;

    limits_free();
    assert_null(limits.circ_buf);

    sem_getvalue(&credits_eps_semaphore, &current_credits);
    assert_int_equal(6, current_credits);

    sem_destroy(&credits_eps_semaphore);
}

// load_limits
void test_load_limits_ok(void ** state)
{
    cJSON *output = cJSON_CreateArray();
    cJSON_AddNumberToObject(output, "timeframe_eps", 5);
    cJSON_AddNumberToObject(output, "max_eps", 100);

    expect_string(__wrap_load_limits_file, daemon_name, "wazuh-analysisd");
    will_return(__wrap_load_limits_file, output);
    will_return(__wrap_load_limits_file, 0);

    expect_string(__wrap__minfo, formatted_msg, "eps limit enabled, eps: '100', timeframe: '5', events per timeframe: '500'");

    load_limits();

    assert_int_equal(limits.max_eps, 500);
    assert_int_equal(limits.eps, 100);
    assert_int_equal(limits.timeframe, 5);
    assert_true(limits.enabled);
}

void test_load_limits_disabled(void ** state)
{
    cJSON *output = cJSON_CreateArray();
    int current_credits;
    sem_init(&credits_eps_semaphore, 0, 5);
    limits_wait_counter = 0;

    expect_string(__wrap_load_limits_file, daemon_name, "wazuh-analysisd");
    will_return(__wrap_load_limits_file, output);
    will_return(__wrap_load_limits_file, -2);

    expect_string(__wrap__minfo, formatted_msg, "eps limit disabled");

    load_limits();

    assert_false(limits.enabled);
    assert_null(limits.circ_buf);

    sem_getvalue(&credits_eps_semaphore, &current_credits);
    assert_int_equal(5, current_credits);
}

void test_load_limits_timeframe_not_found(void ** state)
{
    cJSON *output = cJSON_CreateArray();
    cJSON_AddNumberToObject(output, "max_eps", 100);

    expect_string(__wrap_load_limits_file, daemon_name, "wazuh-analysisd");
    will_return(__wrap_load_limits_file, output);
    will_return(__wrap_load_limits_file, 0);

    expect_string(__wrap__mwarn, formatted_msg, "timeframe not found, dafault value set: '10'");
    expect_string(__wrap__minfo, formatted_msg, "eps limit enabled, eps: '100', timeframe: '10', events per timeframe: '1000'");

    load_limits();

    assert_int_equal(limits.max_eps, 1000);
    assert_int_equal(limits.eps, 100);
    assert_int_equal(limits.timeframe, 10);
    assert_true(limits.enabled);
}

void test_load_limits_timeframe_not_number(void ** state)
{
    cJSON *output = cJSON_CreateArray();
    cJSON_AddNumberToObject(output, "max_eps", 100);
    cJSON_AddStringToObject(output, "timeframe_eps", "5");

    expect_string(__wrap_load_limits_file, daemon_name, "wazuh-analysisd");
    will_return(__wrap_load_limits_file, output);
    will_return(__wrap_load_limits_file, 0);

    expect_string(__wrap__mwarn, formatted_msg, "timeframe not found, dafault value set: '10'");
    expect_string(__wrap__minfo, formatted_msg, "eps limit enabled, eps: '100', timeframe: '10', events per timeframe: '1000'");

    load_limits();

    assert_int_equal(limits.max_eps, 1000);
    assert_int_equal(limits.eps, 100);
    assert_int_equal(limits.timeframe, 10);
    assert_true(limits.enabled);
}

void test_load_limits_timeframe_min_exceeded(void ** state)
{
    cJSON *output = cJSON_CreateArray();
    cJSON_AddNumberToObject(output, "max_eps", 100);
    cJSON_AddNumberToObject(output, "timeframe_eps", 0);

    expect_string(__wrap_load_limits_file, daemon_name, "wazuh-analysisd");
    will_return(__wrap_load_limits_file, output);
    will_return(__wrap_load_limits_file, 0);

    expect_string(__wrap__mwarn, formatted_msg, "timeframe limit exceeded, value set: '1'");
    expect_string(__wrap__minfo, formatted_msg, "eps limit enabled, eps: '100', timeframe: '1', events per timeframe: '100'");

    load_limits();

    assert_int_equal(limits.max_eps, 100);
    assert_int_equal(limits.eps, 100);
    assert_int_equal(limits.timeframe, 1);
    assert_true(limits.enabled);
}

void test_load_limits_timeframe_max_exceeded(void ** state)
{
    cJSON *output = cJSON_CreateArray();
    cJSON_AddNumberToObject(output, "max_eps", 100);
    cJSON_AddNumberToObject(output, "timeframe_eps", 3601);

    expect_string(__wrap_load_limits_file, daemon_name, "wazuh-analysisd");
    will_return(__wrap_load_limits_file, output);
    will_return(__wrap_load_limits_file, 0);

    expect_string(__wrap__mwarn, formatted_msg, "timeframe limit exceeded, value set: '3600'");
    expect_string(__wrap__minfo, formatted_msg, "eps limit enabled, eps: '100', timeframe: '3600', events per timeframe: '360000'");

    load_limits();

    assert_int_equal(limits.max_eps, 360000);
    assert_int_equal(limits.eps, 100);
    assert_int_equal(limits.timeframe, 3600);
    assert_true(limits.enabled);
}

void test_load_limits_eps_not_found(void ** state)
{
    cJSON *output = cJSON_CreateArray();
    cJSON_AddNumberToObject(output, "timeframe_eps", 100);
    limits_wait_counter = 0;

    expect_string(__wrap_load_limits_file, daemon_name, "wazuh-analysisd");
    will_return(__wrap_load_limits_file, output);
    will_return(__wrap_load_limits_file, 0);

    expect_string(__wrap__mwarn, formatted_msg, "eps limit not found, value set: '0'");
    expect_string(__wrap__minfo, formatted_msg, "eps limit disabled");

    load_limits();

    assert_int_equal(limits_wait_counter, 0);
    assert_int_equal(limits.eps, 0);
    assert_false(limits.enabled);
}

void test_load_limits_eps_not_number(void ** state)
{
    cJSON *output = cJSON_CreateArray();
    cJSON_AddStringToObject(output, "max_eps", "100");
    cJSON_AddNumberToObject(output, "timeframe_eps", 100);
    limits_wait_counter = 0;

    expect_string(__wrap_load_limits_file, daemon_name, "wazuh-analysisd");
    will_return(__wrap_load_limits_file, output);
    will_return(__wrap_load_limits_file, 0);

    expect_string(__wrap__mwarn, formatted_msg, "eps limit not found, value set: '0'");
    expect_string(__wrap__minfo, formatted_msg, "eps limit disabled");

    load_limits();

    assert_int_equal(limits_wait_counter, 0);
    assert_int_equal(limits.eps, 0);
    assert_false(limits.enabled);
}

void test_load_limits_eps_min_exceeded(void ** state)
{
    cJSON *output = cJSON_CreateArray();
    cJSON_AddNumberToObject(output, "max_eps", 0);
    cJSON_AddNumberToObject(output, "timeframe_eps", 10);
    limits_wait_counter = 0;

    expect_string(__wrap_load_limits_file, daemon_name, "wazuh-analysisd");
    will_return(__wrap_load_limits_file, output);
    will_return(__wrap_load_limits_file, 0);

    expect_string(__wrap__minfo, formatted_msg, "eps limit disabled");

    load_limits();

    assert_int_equal(limits_wait_counter, 0);
    assert_int_equal(limits.eps, 0);
    assert_false(limits.enabled);
}

void test_load_limits_eps_max_exceeded(void ** state)
{
    cJSON *output = cJSON_CreateArray();
    cJSON_AddNumberToObject(output, "max_eps", 100001);
    cJSON_AddNumberToObject(output, "timeframe_eps", 10);

    expect_string(__wrap_load_limits_file, daemon_name, "wazuh-analysisd");
    will_return(__wrap_load_limits_file, output);
    will_return(__wrap_load_limits_file, 0);

    expect_string(__wrap__mwarn, formatted_msg, "eps limit exceeded, value set: '100000'");
    expect_string(__wrap__minfo, formatted_msg, "eps limit enabled, eps: '100000', timeframe: '10', events per timeframe: '1000000'");

    load_limits();

    assert_int_equal(limits.max_eps, 1000000);
    assert_int_equal(limits.eps, 100000);
    assert_int_equal(limits.timeframe, 10);
    assert_true(limits.enabled);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        // Tests generate_eps_credits
        cmocka_unit_test(test_generate_eps_credits_ok),
        cmocka_unit_test(test_generate_eps_credits_ok_zero),
        // Tests clean_eps_credits
        cmocka_unit_test(test_clean_eps_credits_ok),
        cmocka_unit_test(test_clean_eps_credits_ok_3),
        // Test increase_event_counter
        cmocka_unit_test_setup_teardown(test_increase_event_counter_ok, test_setup, test_teardown),
        // Test inc_wait_counter
        cmocka_unit_test_setup_teardown(test_inc_wait_counter_ok, test_setup, test_teardown),
        // Test dec_wait_counter
        cmocka_unit_test_setup_teardown(test_dec_wait_counter_ok, test_setup, test_teardown),
        // Test get_eps_credit
        cmocka_unit_test_setup_teardown(test_get_eps_credit_ok, test_setup, test_teardown),
        // Test limits_free
        cmocka_unit_test_setup_teardown(test_limits_free_ok, test_setup, test_teardown),
        // Test load_limits
        cmocka_unit_test_setup_teardown(test_load_limits_ok, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_load_limits_disabled, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_load_limits_timeframe_not_found, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_load_limits_timeframe_not_number, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_load_limits_timeframe_min_exceeded, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_load_limits_timeframe_max_exceeded, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_load_limits_eps_not_found, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_load_limits_eps_not_number, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_load_limits_eps_min_exceeded, test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_load_limits_eps_max_exceeded, test_setup, test_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
