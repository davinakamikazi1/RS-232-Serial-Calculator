#include <C8051F020.h>
#include <string.h>
#include <stdlib.h>

#define BUFFER_SIZE 32

// RS-232 buffers
volatile unsigned char Buffer[BUFFER_SIZE];
volatile unsigned char BufIndex = 0;
volatile bit          BufReady  = 0;

// Function prototypes
void board_init(void);
void process_input(void);
void send_string(const char *s);
void clear_buffer(void);
unsigned long str_to_ulong(const char *str, unsigned char length);

void main(void) {
    WDTCN = 0xDE;   // Disable watchdog
    WDTCN = 0xAD;

    board_init();

    // Banner so we know it’s alive
    send_string("Ready\r\n");

    while (1) {
        if (BufReady) {
            process_input();
            clear_buffer();
            BufReady = 0;
        }
    }
}


void board_init(void) {
    // 1) Crossbar: enable UART0
    XBR2 = 0x40;    // Enable crossbar
    XBR0 = 0x04;    // UART0EN = 1

    // 2) Start external crystal
    OSCXCN = 0x67;               // EXOSC pin enabled, crystal mode
    // Use Timer1 in Mode 2 to generate a ~1 ms delay on the 2 MHz internal RC
    TMOD   = 0x21;               // T1 in mode2, T0 in mode1
    TH1    = (unsigned char)(256 - 167);  
    TR1    = 1;                  // start Timer1
    while (!TF1) ;               // wait for overflow (˜1 ms)
    TR1    = 0;                  
    TF1    = 0;                  
    // now wait for the crystal to settle
    while (!(OSCXCN & 0x80)) ;   // poll XTLVLD
    OSCICN = 0x08;               // switch to external oscillator

    // 3) UART0 @ 9600 baud, 8-bit, variable-baud mode
    //    
    SCON0 = 0x50;                // SM1=1, REN=1
    TH1   = (unsigned char)(-6);
    TR1   = 1;                   // start baud clock

    // 4) Make TX0 (P0.0) push-pull so it actually drives the line
    P0MDOUT |= 0x01;

    // 5) Enable serial interrupts
    TI0   = 1;                   // prime transmit flag
    ES0   = 1;                   
    EA    = 1;                   
}

void UART0_ISR(void) interrupt 4 {
    unsigned char c;

    if (RI0) {
        RI0 = 0;
        c   = SBUF0;

        
        if ((c >= '0' && c <= '9') ||
            c=='+' || c=='-' ||
            c=='*' || c=='/' ||
            c=='=' || c=='\r')
        {
            if (BufIndex < BUFFER_SIZE-1) {
                Buffer[BufIndex++] = c;

                // echo it back
                while (!TI0);
                TI0 = 0;
                SBUF0 = c;

                // trigger on '=' 
                if (c == '=' || c == '\r') {
                    BufReady = 1;
                }
            }
        }
    }
}


void process_input(void) {

    unsigned char i;
    unsigned char op_pos;
    unsigned char op_found;
    unsigned char len1;
    unsigned char len2;
    unsigned char end;
    unsigned long num1;
    unsigned long num2;
    unsigned long result;
    unsigned long tmp;
    char          op;
    unsigned char idx;
    unsigned char j;
    char          out[12];

    // Initialize counters
    op_pos   = 0;
    op_found = 0;
    len1     = 0;
    len2     = 0;
    idx      = 0;

    // Must end in '=' or CR
    if (BufIndex == 0) {
        send_string("ERROR\r\n");
        return;
    }
    if (Buffer[BufIndex - 1] == '\r') {
        end = BufIndex - 1;
    } else if (Buffer[BufIndex - 1] == '=') {
        end = BufIndex - 1;
    } else {
        send_string("ERROR\r\n");
        return;
    }

    // Scan from 0 to end-1: count digits and find the one operator
    for (i = 0; i < end; i++) {
        char ch = Buffer[i];
        if (ch >= '0' && ch <= '9') {
            if (!op_found) {
                len1++;
            } else {
                len2++;
            }
        }
        else if ((ch == '+' || ch == '-' || ch == '*' || ch == '/') && !op_found && i > 0) {
            op_found = 1;
            op_pos   = i;
            op       = ch;
        }
        else {
            send_string("ERROR\r\n");
            return;
        }
    }

    // check that exactly one operator and both operands have length >= 1
    if (!op_found || len1 == 0 || len2 == 0) {
        send_string("ERROR\r\n");
        return;
    }

    // Convert ASCII substrings to unsigned longs
    num1 = str_to_ulong(Buffer,              op_pos);
    num2 = str_to_ulong(Buffer + op_pos + 1, len2);

    // Perform calculation with overflow / divide-by-zero checks
    switch (op) {
        case '+':
            if (num1 > 0xFFFFFFFFUL - num2) {
                send_string("OVERFLOW\r\n");
                return;
            }
            result = num1 + num2;
            break;
        case '-':
            if (num1 < num2) {
                send_string("OVERFLOW\r\n");
                return;
            }
            result = num1 - num2;
            break;
        case '*':
            if (num2 && num1 > 0xFFFFFFFFUL / num2) {
                send_string("OVERFLOW\r\n");
                return;
            }
            result = num1 * num2;
            break;
        case '/':
            if (num2 == 0) {
                send_string("ERROR\r\n");
                return;
            }
            result = num1 / num2;
            break;
        default:
            // should never happen
            send_string("ERROR\r\n");
            return;
    }

    // Convert result to ASCII in 'out' buffer, then append "\r\n"
    if (result == 0) {
        out[idx++] = '0';
    } else {
        tmp = result;
        while (tmp > 0) {
            out[idx++] = '0' + (tmp % 10);
            tmp /= 10;
        }
        // Reverse the digits in-place
        for (j = 0; j < idx/2; j++) {
            char t        = out[j];
            out[j]        = out[idx - 1 - j];
            out[idx - 1 - j] = t;
        }
    }
    out[idx++] = '\r';
    out[idx++] = '\n';
    out[idx]   = '\0';

    // Send the resulting string
    send_string(out);
}


// Send a NUL-terminated string over UART0
void send_string(const char *s) {
    // no locals
    while (*s) {
        while (!TI0);
        TI0 = 0;
        SBUF0 = *s++;
    }
}

// Reset buffer for next line
void clear_buffer(void) {
    // no locals
    BufIndex = 0;
    memset((void*)Buffer, 0, BUFFER_SIZE);
}

// Convert ASCII digits to unsigned long
unsigned long str_to_ulong(const char *str, unsigned char length) {
    unsigned long v = 0;
    unsigned char i;
    for (i = 0; i < length; i++) {
        v = v * 10 + (str[i] - '0');
    }
    return v;
}
