/*
 * demo.h  –  NABTS demo mode interface
 *
 * demo_graphics()  NAPLPS colour bars, bouncing box, frame counter
 * demo_ascii()     plain ASCII identification packets, continuous nulls
 * demo()           alias for demo_graphics()
 */
#ifndef DEMO_H
#define DEMO_H

typedef enum {
    DEMO_GRAPHICS = 0,
    DEMO_ASCII    = 1
} DemoMode;

void demo_graphics(void);
void demo_ascii(void);
void demo(void);

#endif /* DEMO_H */
