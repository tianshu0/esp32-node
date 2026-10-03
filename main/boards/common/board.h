#pragma once

#include <string>

void* create_board();

class Display;

class Board {

private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();
    // 软件生成的设备唯一标识
    std::string uuid_;

public:

    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;

    // 板载显示对象；具体板型在构造函数内创建并 override 返回。
    // 默认返回 NoDisplay（无屏面板兜底）。
    virtual Display* GetDisplay();
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}
