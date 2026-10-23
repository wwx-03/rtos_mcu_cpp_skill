#pragma once

class Device;
class DisplayPanel;
class Board;

Board *create_new_board();

class Board {
public:
	static Board &GetInstance() {
		static Board *instance = create_new_board();
		return *instance;
	}

	virtual bool Init() = 0;
	virtual Device *GetDevice() = 0;
	virtual DisplayPanel *GetDisplayPanel() = 0;

protected:
	Board() = default;
	~Board() = default;
	Board(const Board &) = delete;
	Board &operator=(const Board &) = delete;
};

#define IMPLEMENT_BOARD_CLASS(class_name) \
	Board *create_new_board() {              \
		static class_name instance;          \
		return &instance;                    \
	}
