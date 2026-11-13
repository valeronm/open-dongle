#include "test.h"
#include "protocol.h"

static void test_a_stored_protocol_is_used(void)
{
	CHECK_EQ(protocol_from_stored(true, PROTOCOL_NUPHY), PROTOCOL_NUPHY);
	CHECK_EQ(protocol_from_stored(true, PROTOCOL_UNIFYING), PROTOCOL_UNIFYING);
}

static void test_nothing_stored_runs_nuphy(void)
{
	CHECK_EQ(protocol_from_stored(false, PROTOCOL_UNIFYING), PROTOCOL_NUPHY);
}

static void test_an_unknown_stored_value_runs_nuphy(void)
{
	CHECK_EQ(protocol_from_stored(true, 0), PROTOCOL_NUPHY);
	CHECK_EQ(protocol_from_stored(true, 3), PROTOCOL_NUPHY);
	CHECK_EQ(protocol_from_stored(true, 0xff), PROTOCOL_NUPHY);
}

static void test_an_arg_names_a_protocol_or_none(void)
{
	enum protocol p = PROTOCOL_NUPHY;

	CHECK(protocol_from_arg(2, &p));
	CHECK_EQ(p, PROTOCOL_UNIFYING);
	CHECK(protocol_from_arg(1, &p));
	CHECK_EQ(p, PROTOCOL_NUPHY);
	CHECK(!protocol_from_arg(0, &p));
	CHECK(!protocol_from_arg(3, &p));
	CHECK(!protocol_from_arg(0x101, &p));
}

int main(void)
{
	RUN(test_a_stored_protocol_is_used);
	RUN(test_nothing_stored_runs_nuphy);
	RUN(test_an_unknown_stored_value_runs_nuphy);
	RUN(test_an_arg_names_a_protocol_or_none);
	return TEST_EXIT();
}
