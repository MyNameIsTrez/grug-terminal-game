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

static void handle_poison(human *human) {
	if (human->poison.ticks_left > 0) {
		change_human_health(human->id, -human->poison.damage_per_tick);
		human->poison.ticks_left--;
	}
}

static void push_file_containing_fn(grug_file_t file) {
	if (data.type_files_size + 1 > MAX_TYPE_FILES) {
		fprintf(stderr, "There are more than %d files containing the requested type, exceeding MAX_TYPE_FILES", MAX_TYPE_FILES);
		exit(EXIT_FAILURE);
	}
	data.type_files[data.type_files_size++] = file;
}

static void get_type_files_impl(grug_mod_dir_t dir, char *fn_name) {
	for (size_t i = 0; i < dir.dirs_size; i++) {
		get_type_files_impl(dir.dirs[i], fn_name);
	}
	for (size_t i = 0; i < dir.files_size; i++) {
		if (strcmp(fn_name, dir.files[i].define_type) == 0) {
			push_file_containing_fn(dir.files[i]);
		}
	}
}

static grug_file_t *get_type_files(char *fn_name) {
	data.type_files_size = 0;
	get_type_files_impl(grug_mods, fn_name);
	return data.type_files;
}

static void fight() {
	human *player = &data.humans[PLAYER_INDEX];
	human *opponent = &data.humans[OPPONENT_INDEX];

	void *player_tool_globals = data.tool_globals[PLAYER_INDEX];
	void *opponent_tool_globals = data.tool_globals[OPPONENT_INDEX];

	tool *player_tool = &data.tools[PLAYER_INDEX];
	tool *opponent_tool = &data.tools[OPPONENT_INDEX];

	printf("You have %d health\n", player->health);
	printf("The opponent has %d health\n\n", opponent->health);

	typeof(on_tool_use) *use = player_tool->on_fns->use;
	if (use) {
		printf("You use your %s\n", player_tool->name);
		sleep(1);
		use(player_tool_globals, *player_tool);
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

	use = opponent_tool->on_fns->use;
	if (use) {
		printf("The opponent uses their %s\n", opponent_tool->name);
		sleep(1);
		use(opponent_tool_globals, *opponent_tool);
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

static void print_opponent_humans(grug_file_t *files_defining_human) {
	for (size_t i = 0; i < data.type_files_size; i++) {
		human human = *(struct human *)files_defining_human[i].define;
		printf("%ld. %s, worth %d gold when killed\n", i + 1, human.name, human.kill_gold_value);
	}
	printf("\n");
}

static void pick_opponent() {
	printf("You have %d gold\n\n", data.gold);

	grug_file_t *files_defining_human = get_type_files("human");

	print_opponent_humans(files_defining_human);

	printf("Type the number next to the human you want to fight:\n");

	size_t opponent_number;
	if (!read_size(&opponent_number)) {
		return;
	}

	if (opponent_number == 0) {
		fprintf(stderr, "The minimum number you can enter is 1\n");
		return;
	}
	if (opponent_number > data.type_files_size) {
		fprintf(stderr, "The maximum number you can enter is %ld\n", data.type_files_size);
		return;
	}

	size_t opponent_index = opponent_number - 1;

	grug_file_t file = files_defining_human[opponent_index];

	human human = *(struct human *)file.define;

	human.id = OPPONENT_INDEX;
	human.opponent_id = PLAYER_INDEX;

	human.max_health = human.health;

	data.humans[OPPONENT_INDEX] = human;
	data.human_dlls[OPPONENT_INDEX] = file.dll;

	free(data.human_globals[OPPONENT_INDEX]);
	data.human_globals[OPPONENT_INDEX] = malloc(file.globals_struct_size);
	file.init_globals_struct_fn(data.human_globals[OPPONENT_INDEX]);

	// Give the opponent a random tool
	grug_file_t *files_defining_tool = get_type_files("tool");
	size_t tool_index = rand() % data.type_files_size;

	file = files_defining_tool[tool_index];

	tool tool = *(struct tool *)file.define;

	tool.on_fns = file.on_fns;

	tool.human_parent_id = OPPONENT_INDEX;

	data.tools[OPPONENT_INDEX] = tool;
	data.tool_dlls[OPPONENT_INDEX] = file.dll;

	free(data.tool_globals[OPPONENT_INDEX]);
	data.tool_globals[OPPONENT_INDEX] = malloc(file.globals_struct_size);
	file.init_globals_struct_fn(data.tool_globals[OPPONENT_INDEX]);

	data.state = STATE_FIGHTING;
}

static void print_tools(grug_file_t *files_defining_tool) {
	for (size_t i = 0; i < data.type_files_size; i++) {
		tool tool = *(struct tool *)files_defining_tool[i].define;
		printf("%ld. %s costs %d gold\n", i + 1, tool.name, tool.buy_gold_value);
	}
	printf("\n");
}

static void pick_tools() {
	printf("You have %d gold\n\n", data.gold);

	grug_file_t *files_defining_tool = get_type_files("tool");

	print_tools(files_defining_tool);

	printf("Type the number next to the tool you want to buy%s:\n", data.player_has_tool ? " (type 0 to skip)" : "");

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
	if (tool_number > data.type_files_size) {
		fprintf(stderr, "The maximum number you can enter is %ld\n", data.type_files_size);
		return;
	}

	size_t tool_index = tool_number - 1;

	grug_file_t file = files_defining_tool[tool_index];

	tool tool = *(struct tool *)file.define;

	tool.on_fns = file.on_fns;

	if (tool.buy_gold_value > data.gold) {
		fprintf(stderr, "You don't have enough gold to buy that tool\n");
		return;
	}

	data.gold -= tool.buy_gold_value;

	tool.human_parent_id = PLAYER_INDEX;

	data.tools[PLAYER_INDEX] = tool;
	data.tool_dlls[PLAYER_INDEX] = file.dll;

	free(data.tool_globals[PLAYER_INDEX]);
	data.tool_globals[PLAYER_INDEX] = malloc(file.globals_struct_size);
	file.init_globals_struct_fn(data.tool_globals[PLAYER_INDEX]);

	data.player_has_tool = true;
}

static void print_playable_humans(grug_file_t *files_defining_human) {
	for (size_t i = 0; i < data.type_files_size; i++) {
		human human = *(struct human *)files_defining_human[i].define;
		printf("%ld. %s, costing %d gold\n", i + 1, human.name, human.buy_gold_value);
	}
	printf("\n");
}

static void pick_player() {
	printf("You have %d gold\n\n", data.gold);

	grug_file_t *files_defining_human = get_type_files("human");

	print_playable_humans(files_defining_human);

	printf("Type the number next to the human you want to play as%s:\n", data.player_has_human ? " (type 0 to skip)" : "");

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
	if (player_number > data.type_files_size) {
		fprintf(stderr, "The maximum number you can enter is %ld\n", data.type_files_size);
		return;
	}

	size_t player_index = player_number - 1;

	grug_file_t file = files_defining_human[player_index];

	human human = *(struct human *)file.define;

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

	free(data.human_globals[PLAYER_INDEX]);
	data.human_globals[PLAYER_INDEX] = malloc(file.globals_struct_size);
	file.init_globals_struct_fn(data.human_globals[PLAYER_INDEX]);

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

int main() {
	srand(time(NULL)); // Seed the random number generator with the number of seconds since 1970

	init_data();

	while (true) {
		if (grug_reload_modified_mods()) {
			fprintf(stderr, "%s in %s:%d\n", grug_error.msg, grug_error.filename, grug_error.line_number);
			exit(EXIT_FAILURE);
		}

		for (size_t reload_index = 0; reload_index < grug_reloads_size; reload_index++) {
			grug_reload_t reload = grug_reloads[reload_index];

			for (size_t i = 0; i < 2; i++) {
				if (reload.old_dll == data.human_dlls[i]) {
					data.human_dlls[i] = reload.new_dll;

					free(data.human_globals[i]);
					data.human_globals[i] = malloc(reload.globals_struct_size);
					reload.init_globals_struct_fn(data.human_globals[i]);
				}
			}
			for (size_t i = 0; i < 2; i++) {
				if (reload.old_dll == data.tool_dlls[i]) {
					data.tool_dlls[i] = reload.new_dll;

					free(data.tool_globals[i]);
					data.tool_globals[i] = malloc(reload.globals_struct_size);
					reload.init_globals_struct_fn(data.tool_globals[i]);

					data.tools[i].on_fns = reload.on_fns;
				}
			}
		}

		// grug_print_mods();
		// printf("\n");

		update();

		printf("\n");

		sleep(1);
	}

	grug_free_mods();
	free_data();
}
