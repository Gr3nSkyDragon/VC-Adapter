/* Board setup that has to happen before Wi-Fi starts, in either mode (board.c). */
#ifndef UDS_BOARD_H
#define UDS_BOARD_H

/* First thing in app_main. Does nothing on boards that need nothing. */
void board_init(void);

#endif
