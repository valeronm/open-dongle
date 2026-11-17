#include <string.h>
#include "btlist.h"
#include "test.h"

static const uint8_t mx[BTPOLICY_ADDR_LEN] = {1, 0xd9, 0x9a, 0xa6, 0x49, 0xbb, 0xeb};
static const uint8_t air[BTPOLICY_ADDR_LEN] = {1, 0x24, 0xa8, 0x4b, 0x3c, 0x7d, 0xd6};
static const uint8_t old[BTPOLICY_ADDR_LEN] = {1, 0xd8, 0x9a, 0xa6, 0x49, 0xbb, 0xeb};

static bool is(const struct btlist_entry *e, const uint8_t *addr, const char *name)
{
	char want[BTLIST_NAME_LEN] = {0};

	strncpy(want, name, sizeof(want));
	return memcmp(e->addr, addr, BTPOLICY_ADDR_LEN) == 0 &&
	       memcmp(e->name, want, sizeof(want)) == 0;
}

static void a_connecting_keyboard_moves_to_the_front_with_its_name(void)
{
	struct btlist l = {0};

	CHECK(btlist_connected(&l, mx, "MX Keys"));
	CHECK(btlist_connected(&l, air, "NuPhy Air75 V2-1"));
	CHECK_EQ(l.count, 2);
	CHECK(is(&l.entries[0], air, "NuPhy Air75 V2-1"));
	CHECK(is(&l.entries[1], mx, "MX Keys"));
	CHECK(btlist_connected(&l, mx, "MX Keys"));
	CHECK(is(&l.entries[0], mx, "MX Keys"));
	CHECK(is(&l.entries[1], air, "NuPhy Air75 V2-1"));
	CHECK_EQ(l.count, 2);
}

static void connecting_again_unchanged_changes_nothing(void)
{
	struct btlist l = {0};

	btlist_connected(&l, mx, "MX Keys");
	CHECK(!btlist_connected(&l, mx, "MX Keys"));
	CHECK(btlist_connected(&l, mx, "MX Keys S"));
	CHECK(is(&l.entries[0], mx, "MX Keys S"));
}

static void a_long_name_is_cut_to_the_field(void)
{
	struct btlist l = {0};

	btlist_connected(&l, mx, "A keyboard with a very long name indeed");
	CHECK(memcmp(l.entries[0].name, "A keyboard with a very l", BTLIST_NAME_LEN) == 0);
}

static void pairing_records_whether_a_passkey_was_typed(void)
{
	struct btlist l = {0};

	CHECK(btlist_paired(&l, air, true));
	CHECK(btlist_paired(&l, mx, false));
	CHECK_EQ(l.count, 2);
	CHECK(is(&l.entries[0], mx, ""));
	CHECK(!l.entries[0].no_passkey);
	CHECK(l.entries[1].no_passkey);
	CHECK(!btlist_paired(&l, mx, false));
	btlist_connected(&l, air, "NuPhy Air75 V2-1");
	CHECK(l.entries[0].no_passkey);
}

static void removing_closes_the_gap(void)
{
	struct btlist l = {0};

	btlist_connected(&l, old, "MX Keys");
	btlist_connected(&l, air, "NuPhy Air75 V2-1");
	btlist_connected(&l, mx, "MX Keys");
	CHECK(btlist_remove(&l, air));
	CHECK_EQ(l.count, 2);
	CHECK(is(&l.entries[0], mx, "MX Keys"));
	CHECK(is(&l.entries[1], old, "MX Keys"));
	CHECK(!btlist_remove(&l, air));
}

static void reconciling_drops_entries_without_a_bond_and_adds_bonds_without_an_entry(void)
{
	const uint8_t bonded[][BTPOLICY_ADDR_LEN] = {
		{1, 0xd9, 0x9a, 0xa6, 0x49, 0xbb, 0xeb},
		{1, 0xd8, 0x9a, 0xa6, 0x49, 0xbb, 0xeb},
	};
	struct btlist l = {0};

	btlist_connected(&l, mx, "MX Keys");
	btlist_connected(&l, air, "NuPhy Air75 V2-1");
	CHECK(btlist_reconcile(&l, bonded, 2));
	CHECK_EQ(l.count, 2);
	CHECK(is(&l.entries[0], mx, "MX Keys"));
	CHECK(is(&l.entries[1], old, ""));
	CHECK(!btlist_reconcile(&l, bonded, 2));
}

static void a_full_list_takes_no_fifth_keyboard(void)
{
	struct btlist l = {0};
	uint8_t addr[BTPOLICY_ADDR_LEN] = {1};

	for (uint8_t i = 0; i < BTPOLICY_STORED_MAX; i++) {
		addr[1] = i;
		CHECK(btlist_connected(&l, addr, "k"));
	}
	addr[1] = 99;
	CHECK(!btlist_connected(&l, addr, "k"));
	CHECK(!btlist_paired(&l, addr, false));
	CHECK_EQ(l.count, BTPOLICY_STORED_MAX);
}

static void only_a_listed_keyboard_is_in_the_list(void)
{
	struct btlist l = {0};
	const uint8_t mx[BTPOLICY_ADDR_LEN] = {1, 1};
	const uint8_t air[BTPOLICY_ADDR_LEN] = {1, 2};

	btlist_connected(&l, mx, "MX Keys");
	CHECK(btlist_has(&l, mx));
	CHECK(!btlist_has(&l, air));
	btlist_remove(&l, mx);
	CHECK(!btlist_has(&l, mx));
}

int main(void)
{
	RUN(a_connecting_keyboard_moves_to_the_front_with_its_name);
	RUN(connecting_again_unchanged_changes_nothing);
	RUN(a_long_name_is_cut_to_the_field);
	RUN(pairing_records_whether_a_passkey_was_typed);
	RUN(removing_closes_the_gap);
	RUN(reconciling_drops_entries_without_a_bond_and_adds_bonds_without_an_entry);
	RUN(a_full_list_takes_no_fifth_keyboard);
	RUN(only_a_listed_keyboard_is_in_the_list);
	return TEST_EXIT();
}
