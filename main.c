#include "data.h"
#include "game/tool.h"
#include "grug.h"
#include "mod.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

typedef human (*define_human_fn)();
typedef tool (*define_tool_fn)();

static void handle_poison(human *human) {
	if (human->poison.ticks_left > 0) {
		change_human_health(human->id, -human->poison.damage_per_tick);
		human->poison.ticks_left--;
	}
}

static void push_file_containing_fn(grug_file file) {
	if (data.files_containing_fn_size + 1 > MAX_FILES_CONTAINING_FN) {
		fprintf(stderr, "There are more than %d files containing the requested function, exceeding MAX_FILES_CONTAINING_FN", MAX_FILES_CONTAINING_FN);
		exit(EXIT_FAILURE);
	}
	data.files_containing_fn[data.files_containing_fn_size++] = file;
}

static void get_files_containing_fn_impl(mod_directory dir, char *fn_name) {
	for (size_t i = 0; i < dir.dirs_size; i++) {
		get_files_containing_fn_impl(dir.dirs[i], fn_name);
	}
	for (size_t i = 0; i < dir.files_size; i++) {
		if (grug_get_fn(dir.files[i].dll, fn_name)) {
			push_file_containing_fn(dir.files[i]);
		}
	}
}

static grug_file *get_files_containing_fn(char *fn_name) {
	data.files_containing_fn_size = 0;
	get_files_containing_fn_impl(mods, fn_name);
	return data.files_containing_fn;
}

static define_human_fn get_define_human_fn(void *dll) {
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wpedantic"
	return grug_get_fn(dll, "define_human");
	#pragma GCC diagnostic pop
}

static define_tool_fn get_define_tool_fn(void *dll) {
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wpedantic"
	return grug_get_fn(dll, "define_tool");
	#pragma GCC diagnostic pop
}

static typeof(on_tool_use) *get_on_tool_use_fn(void *dll) {
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wpedantic"
	return grug_get_fn(dll, "on_tool_use");
	#pragma GCC diagnostic pop
}

static void fight() {
	human *player = &data.humans[PLAYER_INDEX];
	human *opponent = &data.humans[OPPONENT_INDEX];

	tool *player_tool = &data.tools[PLAYER_INDEX];
	void *player_tool_globals = &data.tool_globals[PLAYER_INDEX];
	tool *opponent_tool = &data.tools[OPPONENT_INDEX];
	void *opponent_tool_globals = &data.tool_globals[OPPONENT_INDEX];

	printf("You have %d health\n", player->health);
	printf("The opponent has %d health\n\n", opponent->health);

	typeof(on_tool_use) *fn = get_on_tool_use_fn(data.tool_dlls[PLAYER_INDEX]);
	if (fn) {
		printf("You use your %s\n", player_tool->name);
		sleep(1);
		fn(player_tool_globals, *player_tool);
		sleep(1);
	} else {
		printf("You don't know what to do with your %s\n", player_tool->name);
		sleep(1);
	}

	handle_poison(opponent);
	if (opponent->health <= 0) {
		printf("The opponent died!\n");
		sleep(1);
		data.state = STATE_PICKING_PLAYER;
		data.gold += opponent->kill_gold_value;
		return;
	}

	fn = get_on_tool_use_fn(data.tool_dlls[OPPONENT_INDEX]);
	if (fn) {
		printf("The opponent uses their %s\n", opponent_tool->name);
		sleep(1);
		fn(opponent_tool_globals, *opponent_tool);
		sleep(1);
	} else {
		printf("The opponent doesn't know what to do with their %s\n", opponent_tool->name);
		sleep(1);
	}

	handle_poison(player);
	if (player->health <= 0) {
		printf("You died!\n");
		sleep(1);
		data.state = STATE_PICKING_PLAYER;
		return;
	}
}

static void discard_unread() {
	int c;
	while ((c = getchar()) != '\n' && c != EOF) {}
}

// Returns true if the input was valid
static bool read_size(size_t *output) {
	char buffer[42];
	if (!fgets(buffer, sizeof(buffer), stdin)) {
		perror("fgets");
		exit(EXIT_FAILURE);
	}

	char *endptr;
	errno = 0;
	long l = strtol(buffer, &endptr, 10);
	if (errno != 0) {
		perror("strtol");
		// This is to prevent the next strtol() call from continuing
		// when the input was for example a long series of "11111111..."
		discard_unread();
		return false;
	} else if (buffer == endptr) {
		fprintf(stderr, "No number was provided\n");
		return false;
	} else if (*endptr != '\n' && *endptr != '\0') {
		fprintf(stderr, "There was an extra character after the number\n");
		return false;
	} else if (l < 0) {
		fprintf(stderr, "You can't enter a negative number\n");
		return false;
	}

	*output = l;

	return true;
}

static void print_opponent_humans(grug_file *files_defining_human) {
	for (size_t i = 0; i < data.files_containing_fn_size; i++) {
		grug_file file = files_defining_human[i];
		human human = get_define_human_fn(file.dll)();
		printf("%ld. %s, worth %d gold when killed\n", i + 1, human.name, human.kill_gold_value);
	}
	printf("\n");
}

static void *allocate_globals(grug_file file) {
	void *globals = malloc(file.globals_struct_size);
	file.init_globals_struct_fn(globals);
	return globals;
}

static void pick_opponent() {
	printf("You have %d gold\n\n", data.gold);

	grug_file *files_defining_human = get_files_containing_fn("define_human");

	print_opponent_humans(files_defining_human);

	printf("Type the number of the human you want to fight:\n");

	size_t opponent_number;
	if (!read_size(&opponent_number)) {
		return;
	}

	if (opponent_number == 0) {
		fprintf(stderr, "The minimum number you can enter is 1\n");
		return;
	}
	if (opponent_number > data.files_containing_fn_size) {
		fprintf(stderr, "The maximum number you can enter is %ld\n", data.files_containing_fn_size);
		return;
	}

	size_t opponent_index = opponent_number - 1;

	grug_file file = files_defining_human[opponent_index];

	human human = get_define_human_fn(file.dll)();

	human.id = OPPONENT_INDEX;
	human.opponent_id = PLAYER_INDEX;

	human.max_health = human.health;

	data.humans[OPPONENT_INDEX] = human;
	data.human_dlls[OPPONENT_INDEX] = file.dll;
	data.human_globals[OPPONENT_INDEX] = allocate_globals(file);

	// Give the opponent a random tool
	grug_file *files_defining_tool = get_files_containing_fn("define_tool");
	size_t tool_index = rand() % data.files_containing_fn_size;

	file = files_defining_tool[tool_index];

	tool tool = get_define_tool_fn(file.dll)();

	tool.human_parent_id = OPPONENT_INDEX;

	data.tools[OPPONENT_INDEX] = tool;
	data.tool_dlls[OPPONENT_INDEX] = file.dll;
	data.tool_globals[OPPONENT_INDEX] = allocate_globals(file);

	data.state = STATE_FIGHTING;
}

static void print_tools(grug_file *files_defining_tool) {
	for (size_t i = 0; i < data.files_containing_fn_size; i++) {
		tool tool = get_define_tool_fn(files_defining_tool[i].dll)();
		printf("%ld. %s costs %d gold\n", i + 1, tool.name, tool.buy_gold_value);
	}
	printf("\n");
}

static void pick_tools() {
	printf("You have %d gold\n\n", data.gold);

	grug_file *files_defining_tool = get_files_containing_fn("define_tool");

	print_tools(files_defining_tool);

	printf("Type the number of any tools you want to buy%s:\n", data.player_has_tool ? " (type 0 to skip)" : "");

	size_t tool_number;
	if (!read_size(&tool_number)) {
		return;
	}

	if (tool_number == 0) {
		if (data.player_has_tool) {
			data.state = STATE_PICKING_OPPONENT;
			return;
		}
		fprintf(stderr, "The minimum number you can enter is 1\n");
		return;
	}
	if (tool_number > data.files_containing_fn_size) {
		fprintf(stderr, "The maximum number you can enter is %ld\n", data.files_containing_fn_size);
		return;
	}

	size_t tool_index = tool_number - 1;

	grug_file file = files_defining_tool[tool_index];

	tool tool = get_define_tool_fn(file.dll)();

	if (tool.buy_gold_value > data.gold) {
		fprintf(stderr, "You don't have enough gold to buy that tool\n");
		return;
	}

	data.gold -= tool.buy_gold_value;

	tool.human_parent_id = PLAYER_INDEX;

	data.tools[PLAYER_INDEX] = tool;
	data.tool_dlls[PLAYER_INDEX] = file.dll;
	data.tool_globals[PLAYER_INDEX] = allocate_globals(file);

	data.player_has_tool = true;
}

static void print_playable_humans(grug_file *files_defining_human) {
	for (size_t i = 0; i < data.files_containing_fn_size; i++) {
		human human = get_define_human_fn(files_defining_human[i].dll)();
		printf("%ld. %s, costing %d gold\n", i + 1, human.name, human.buy_gold_value);
	}
	printf("\n");
}

static void pick_player() {
	printf("You have %d gold\n\n", data.gold);

	grug_file *files_defining_human = get_files_containing_fn("define_human");

	print_playable_humans(files_defining_human);

	printf("Type the number of the human you want to play as%s:\n", data.player_has_human ? " (type 0 to skip)" : "");

	size_t player_number;
	if (!read_size(&player_number)) {
		return;
	}

	if (player_number == 0) {
		if (data.player_has_human) {
			data.state = STATE_PICKING_TOOLS;
			return;
		}
		fprintf(stderr, "The minimum number you can enter is 1\n");
		return;
	}
	if (player_number > data.files_containing_fn_size) {
		fprintf(stderr, "The maximum number you can enter is %ld\n", data.files_containing_fn_size);
		return;
	}

	size_t player_index = player_number - 1;

	grug_file file = files_defining_human[player_index];

	human human = get_define_human_fn(file.dll)();

	if (human.buy_gold_value > data.gold) {
		fprintf(stderr, "You don't have enough gold to pick that human\n");
		return;
	}

	data.gold -= human.buy_gold_value;

	human.id = PLAYER_INDEX;
	human.opponent_id = OPPONENT_INDEX;

	human.max_health = human.health;

	data.humans[PLAYER_INDEX] = human;
	data.human_dlls[PLAYER_INDEX] = file.dll;
	data.human_globals[PLAYER_INDEX] = allocate_globals(file);

	data.player_has_human = true;

	data.state = STATE_PICKING_TOOLS;
}

static void update() {
	switch (data.state) {
	case STATE_PICKING_PLAYER:
		pick_player();
		break;
	case STATE_PICKING_TOOLS:
		pick_tools();
		break;
	case STATE_PICKING_OPPONENT:
		pick_opponent();
		break;
	case STATE_FIGHTING:
		fight();
		break;
	}
}

static void error_handler(char *error_msg, char *filename, int line_number) {
	fprintf(stderr, "%s in %s:%d\n", error_msg, filename, line_number);
	exit(EXIT_FAILURE);
}

int main() {
	srand(time(NULL)); // Seed the random number generator with the number of seconds since 1970

	grug_error_handler = error_handler;

	init_data();

	while (true) {
		grug_free_mods(mods);

		while (grug_reload_modified_mods()) {
			// TODO:
		}

		// grug_print_mods(data.mods);

		update();

		printf("\n");

		sleep(1);
	}
	
	free_data();
}
