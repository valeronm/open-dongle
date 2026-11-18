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

	CHECK(btlist_paired(&l, air, DEVICE_KEYBOARD, true));
	CHECK(btlist_paired(&l, mx, DEVICE_KEYBOARD, false));
	CHECK_EQ(l.count, 2);
	CHECK(is(&l.entries[0], mx, ""));
	CHECK(!l.entries[0].no_passkey);
	CHECK(l.entries[1].no_passkey);
	CHECK(!btlist_paired(&l, mx, DEVICE_KEYBOARD, false));
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

static void a_full_list_takes_no_more_devices(void)
{
	struct btlist l = {0};
	uint8_t addr[BTPOLICY_ADDR_LEN] = {1};

	for (uint8_t i = 0; i < BTPOLICY_STORED_MAX; i++) {
		addr[1] = i;
		CHECK(btlist_connected(&l, addr, "k"));
	}
	addr[1] = 99;
	CHECK(!btlist_connected(&l, addr, "k"));
	CHECK(!btlist_paired(&l, addr, DEVICE_KEYBOARD, false));
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

static const uint8_t mouse[BTPOLICY_ADDR_LEN] = {1, 0x17, 0x3e, 0xff, 0x26, 0x33, 0xd4};

static void pairing_records_the_kind(void)
{
	struct btlist l = {0};

	CHECK(btlist_paired(&l, mouse, DEVICE_MOUSE, true));
	CHECK(btlist_paired(&l, mx, DEVICE_KEYBOARD, false));
	CHECK_EQ(btlist_kind(&l, mouse), DEVICE_MOUSE);
	CHECK_EQ(btlist_kind(&l, mx), DEVICE_KEYBOARD);
	CHECK_EQ(btlist_kind(&l, air), DEVICE_NO_KIND);
	CHECK(btlist_connected(&l, mouse, "MX Master 3S"));
	CHECK_EQ(btlist_kind(&l, mouse), DEVICE_MOUSE);
}

static void a_bond_without_an_entry_is_listed_as_a_keyboard(void)
{
	const uint8_t bonded[][BTPOLICY_ADDR_LEN] = {{1, 0x17, 0x3e, 0xff, 0x26, 0x33, 0xd4}};
	struct btlist l = {0};

	CHECK(btlist_reconcile(&l, bonded, 1));
	CHECK_EQ(btlist_kind(&l, mouse), DEVICE_KEYBOARD);
}

static void a_kind_is_stored_once_a_device_of_it_is_listed(void)
{
	struct btlist l = {0};

	CHECK(!btlist_has_kind(&l, DEVICE_KEYBOARD));
	btlist_paired(&l, mouse, DEVICE_MOUSE, true);
	CHECK(btlist_has_kind(&l, DEVICE_MOUSE));
	CHECK(!btlist_has_kind(&l, DEVICE_KEYBOARD));
	btlist_paired(&l, mx, DEVICE_KEYBOARD, false);
	CHECK(btlist_has_kind(&l, DEVICE_KEYBOARD));
	btlist_remove(&l, mouse);
	CHECK(!btlist_has_kind(&l, DEVICE_MOUSE));
}

static bool latest_is(const struct btlist *l, enum device_kind kind, const uint8_t *addr,
		      const char *name)
{
	const struct btlist_entry *e = btlist_latest(l, kind);

	return e != NULL && is(e, addr, name);
}

static void the_latest_of_a_kind_is_the_most_recently_connected(void)
{
	struct btlist l = {0};

	CHECK(btlist_latest(&l, DEVICE_KEYBOARD) == NULL);
	btlist_paired(&l, mouse, DEVICE_MOUSE, true);
	btlist_connected(&l, mouse, "MX Master 3S");
	btlist_paired(&l, air, DEVICE_KEYBOARD, true);
	btlist_connected(&l, air, "Air75");
	btlist_paired(&l, mx, DEVICE_KEYBOARD, false);
	btlist_connected(&l, mx, "MX Keys");
	CHECK(latest_is(&l, DEVICE_KEYBOARD, mx, "MX Keys"));
	btlist_connected(&l, air, "Air75");
	CHECK(latest_is(&l, DEVICE_KEYBOARD, air, "Air75"));
	CHECK(latest_is(&l, DEVICE_MOUSE, mouse, "MX Master 3S"));
}

static void eight_devices_are_stored(void)
{
	struct btlist l = {0};
	uint8_t addr[BTPOLICY_ADDR_LEN] = {1};

	for (uint8_t i = 0; i < 8; i++) {
		addr[1] = i;
		btlist_connected(&l, addr, "d");
	}
	CHECK_EQ(l.count, 8);
	CHECK(btlist_has(&l, (const uint8_t[BTPOLICY_ADDR_LEN]){1, 0}));
}

int main(void)
{
	RUN(a_connecting_keyboard_moves_to_the_front_with_its_name);
	RUN(connecting_again_unchanged_changes_nothing);
	RUN(a_long_name_is_cut_to_the_field);
	RUN(pairing_records_whether_a_passkey_was_typed);
	RUN(removing_closes_the_gap);
	RUN(reconciling_drops_entries_without_a_bond_and_adds_bonds_without_an_entry);
	RUN(a_full_list_takes_no_more_devices);
	RUN(only_a_listed_keyboard_is_in_the_list);
	RUN(the_latest_of_a_kind_is_the_most_recently_connected);
	RUN(eight_devices_are_stored);
	RUN(pairing_records_the_kind);
	RUN(a_bond_without_an_entry_is_listed_as_a_keyboard);
	RUN(a_kind_is_stored_once_a_device_of_it_is_listed);
	return TEST_EXIT();
}
