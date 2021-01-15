/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "OnScreenKeyboard.h"
#include "boot.h"
#include "video.h"
#include "lib/time/timeManagement.h"
#include "lib/cromwell/cromSystem.h"
#include "lib/cromwell/cromString.h"
#include "string.h"

#define FULL_KEYBOARD   0
#define IP_KEYPAD       1
#define HEX_KEYPAD      2

#define KeymapNumberOfRows 4
#define KeymapNumberOfColumns 10

const char keymap[KeymapNumberOfRows][KeymapNumberOfColumns] =
{
    {'1','2','3','4','5','6','7','8','9','0'},
    {'A','B','C','D','E','F','G','H','I','J'},
    {'K','L','M','N','O','P','Q','R','S','T'},
    {'U','V','W','X','Y','Z',',','.','\'',';'}
};

const char shiftkeymap[KeymapNumberOfRows][KeymapNumberOfColumns] =
{
    {'!','@','#','$','%','?','&','*','(',')'},
    {'a','b','c','d','e','f','g','h','i','j'},
    {'k','l','m','n','o','p','q','r','s','t'},
    {'u','v','w','x','y','z','-','+','\"',':'}
};

#define IPKeypadNumberOfRows 4
#define IPKeypadNumberOfColumns 3

const char ipKeypad[IPKeypadNumberOfRows][IPKeypadNumberOfColumns] =
{
    {'1','2','3'},
    {'4','5','6'},
    {'7','8','9'},
    {' ','0','.'}
};

#define HexKeypadNumberOfRows 4
#define HexKeypadNumberOfColumns 4

const char hexKeypad[HexKeypadNumberOfRows][HexKeypadNumberOfColumns] =
{
    {'A','1','2','3'},
    {'B','4','5','6'},
    {'C','7','8','9'},
    {'D','E','F','0'}
};

unsigned short myAtoi(const char *str)
{
    unsigned char i;
    unsigned short res = 0;

    for (i = 0; str[i] != '\0' && str[i] != '.'; ++i)   //Will stop converting if it hit a '.' or the end of the string.
        res = res*10 + str[i] - '0';

    return res;
}

void OnScreenKeyboard(char * string, unsigned char maxLength, unsigned char line, unsigned char kbType)
{
    bool exit = false;
    unsigned char cursorposX = 0;
    unsigned char cursorposY = 0;
    unsigned char textpos = 0;
    unsigned char x, y, i;
    bool shift = false;
    bool refresh = true;
    bool charAccepted = false;
    unsigned char rowLength, stringLength;
    char ipFieldLength, dotCount;     //Assume IP string is properly constructed.
    char limitFieldString[4] = {0, 0, 0, 0};    //To check if user enters a number bigger than 255 in IP Keypad.
    //Array of function pointers to let "line" value decide which function needs to be called.
    char oldString[20];

    strcpy(oldString, string);	//Copy input string in case user cancels.
    textpos = strlen(string);           //Place cursor at end of entering string.
    
    if(kbType == IP_KEYPAD)
    {
        rowLength = IPKeypadNumberOfColumns;
    }
    else if(kbType == HEX_KEYPAD)
    {
        rowLength = HexKeypadNumberOfColumns;
    }
    else
    {
        rowLength = KeymapNumberOfColumns; //Full keyboard by default;
    }

    while(cromwellLoop())
    {
        if(refresh)
        {
            BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);

            VIDEO_CURSOR_POSY=56;
            VIDEO_ARGB=0xffffffff;                        //White characters.
            if(kbType == IP_KEYPAD || kbType == HEX_KEYPAD)
            {
                centerScreenPrintk(VIDEO_CURSOR_POSY, "Back=Cancel   Start=Confirm   B=Backspace");
            }
            else
            {
                centerScreenPrintk(VIDEO_CURSOR_POSY, "Back=Cancel   Start=Confirm   B=Backspace   X=Space   Y=Shift");
            }

            VIDEO_ARGB=0xffff9f00;                	  //Orangeish
            VIDEO_CURSOR_POSX=75;
            VIDEO_CURSOR_POSY=50;
            if(kbType == IP_KEYPAD || kbType == HEX_KEYPAD)
            {
                printk("\n\n\n\n\2                        %s", string);
            }
            else
            {
                printk("\n\n\n\n\2                 %s", string);
            }

            VIDEO_ARGB=0xffffffff;
            if(kbType == IP_KEYPAD)
            {
                printk("\n\n\n                                  ");
            }
            else if(kbType == HEX_KEYPAD)
            {
                printk("\n\n\n                                     ");
            }
            else
            {
                printk("\n\n\n           ");
            }

            for(y = 0; y < 4; y++)
            {
                if(kbType == IP_KEYPAD)
                {
                    if(y == 3)
                    {
                        printk("\n\n\n                                                ");      //Add extra space to align with other rows
                    }
                    else
                    {
                        printk("\n\n\n                                              ");
                    }
                }
                else if(kbType == HEX_KEYPAD)
                {
                                printk("\n\n\n                                  ");
                }
                else
                {
                    printk("\n\n\n           ");
                }

                for(x = 0; x < rowLength; x++)
                {
                    if(x == cursorposX && y == cursorposY)      //About to draw selected character
                    {
                        VIDEO_ARGB=0xffffef37;                  //In yellow
                    }
                    else
                    {
                        VIDEO_ARGB=0xffffffff;                  //the rest in white.
                    }

                    if(kbType == IP_KEYPAD)
                    {
                        printk("\2%c    ",ipKeypad[y][x]);
                    }
                    else if(kbType == HEX_KEYPAD)
                    {
                        printk("\2%c    ",hexKeypad[y][x]);
                    }
                    else
                    {
                        if(shift)
                        {
                            printk("\2%c    ",shiftkeymap[y][x]);
                        }
                        else
                        {
                            printk("\2%c    ",keymap[y][x]);
                        }
                    }
                }
            }

            // if(xLCD.enable == 1 && xLCD.LineOwner[line] != SCRIPT_OWNER)
            // {
            //     xLCD.PrintLine[line](JUSTIFYLEFT, string);
            // }
            refresh = false;
        }
       
        if(risefall_xpad_STATE(XPAD_STATE_BACK))       //Cancel
        {
            strcpy(string, oldString);
    	    exit = true;
        }

        if(risefall_xpad_STATE(XPAD_STATE_START))      //Accept
        {
            if(textpos < maxLength)
                string[textpos] = '\0';
            exit = true;
        }

        if(kbType != IP_KEYPAD && kbType != HEX_KEYPAD)
        {
            if(risefall_xpad_BUTTON(TRIGGER_XPAD_KEY_Y) == 1)   //Shift toggle
            {
                shift = !shift;
                refresh = true;
            }
        }

        if(kbType != IP_KEYPAD && kbType != HEX_KEYPAD)
        {
            if(risefall_xpad_BUTTON(TRIGGER_XPAD_KEY_X) == 1) //Space
            {
                if(textpos < maxLength)
                {
                    string[textpos] = ' ';                  //Add space character
                    textpos += 1;                           //Move cursor one position to the right
                    refresh = true;
                }
            }
        }

        if(risefall_xpad_BUTTON(TRIGGER_XPAD_KEY_B) == 1)   //Backspace
        {
            if(textpos > 0)                                       //Full keyboard
            {
                textpos -= 1;                               //Move cursor one position to the left
                if(kbType == IP_KEYPAD)
                {
                    if(textpos > 2 && string[textpos - 1] == '0' && string[textpos - 2] == '.')
                    {
                        string[textpos] = '\0';
                        textpos -= 1;
                    }                    
                }
                string[textpos] = '\0';                     //Erase character
                refresh = true;
            }
        }

        if(risefall_xpad_BUTTON(TRIGGER_XPAD_KEY_A) == 1)   //Select character
        {
            charAccepted = false;
            if(textpos < maxLength)
            {
                if(kbType == IP_KEYPAD)
                {
                    ipFieldLength = 0;
                    if(textpos > 0){
                        if(string[textpos - 1] != '.')       //IP field already contains 1 digit, we're currently adding another so total length will be 2.
                        {
                            ipFieldLength++;
                            if(textpos > 1)
                            {
                                if(string[textpos - 2] != '.')   //IP field already contains 2 digits, we're currently adding another so total length will be 3.
                                {
                                    ipFieldLength++;
                                    if(textpos > 3)
                                    {
                                        if(string[textpos - 3] != '.')   //IP field already contains 3 digits max digit per field. Useful only for last IP field.
                                        {
                                            ipFieldLength++;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if((textpos == 0 && ipKeypad[cursorposY][cursorposX] != '.' && ipKeypad[cursorposY][cursorposX] != '0') || (textpos > 0))     //Don't start string with a '.' or '0'
                    {
                        if((string[textpos - 1] == '.' && ipKeypad[cursorposY][cursorposX] != '.') ||   //Don't put 2 successive '.'
                           (string[textpos - 1] != '.'))                                                //Selected character is not a '.'. Let it go through.
                        {
                            stringLength = strlen(string);
                            dotCount = 0;
                            for(i = 0; i < stringLength; i++)
                            {
                                dotCount += (string[i] == '.');
                            }

                            if(dotCount < 3 &&  //if not in the last IP field.
                               (ipFieldLength == 2 || //And field will contain 3 digit(with current character input)
                                (ipKeypad[cursorposY][cursorposX] == '0' && string[textpos - 1] == '.')))  //or '0' is entered just after a '.'.
                            {
                                charAccepted = true;
                                string[textpos] = ipKeypad[cursorposY][cursorposX];
                                string[textpos + 1] = '.';     //add '.' automatically
                                string[textpos + 2] = '\0';
                            }
                            else if(dotCount <= 3 && ipFieldLength <= 2)     //or last possible character to write
                            {
                                charAccepted = true;
                                string[textpos] = ipKeypad[cursorposY][cursorposX];
                                string[textpos + 1] = '\0';               //Safe in this situation. Will not write outside buffer because of maxLength's check.
                            }

                            //Check to see if entered value for current field is bigger than 255.
                            memset(limitFieldString, 0, 4);     //Clear buffer.
                            strncpy(limitFieldString, &string[textpos - ipFieldLength], ipFieldLength + 1);     //Copy field
                            if(myAtoi(limitFieldString) > 255)
                            {
                                strncpy(&string[textpos - ipFieldLength], "255", ipFieldLength + 1);    //Either that or cancel character input.
                            }
                        }
                    }
                    else
                    {
                        charAccepted = false;
                    }
                }
                else if(kbType == HEX_KEYPAD)
                {
                    charAccepted = true;
                    string[textpos] = hexKeypad[cursorposY][cursorposX];
                }
                else
                {
                    charAccepted = true;
                    if(shift)
                    {
                        string[textpos] = shiftkeymap[cursorposY][cursorposX];
                    }
                    else
                    {
                        string[textpos] = keymap[cursorposY][cursorposX];
                    }
                }
                refresh = true;
            }

            if(charAccepted)
            {
                if(textpos < maxLength - 1)
                {
            	    textpos += 1;
                }
                if(string[textpos] == '.' && kbType == IP_KEYPAD)   //Moving cursor forward, we stumbled on a dot. just skip to next digit.
                {
                    textpos += 1;
                }
            }
        }

        if (risefall_xpad_BUTTON(TRIGGER_XPAD_PAD_UP) == 1)
        {
            if(cursorposY == 0)         //Already at the top line
            {
            	if(kbType == IP_KEYPAD && cursorposX == 0)
            	{
            	    cursorposY = 2;
            	}
            	else
            	{
                    cursorposY = 3;         //Roll to last
            	}
            }
            else
            {
                cursorposY -= 1;
            }
            refresh = true;
        }

        if (risefall_xpad_BUTTON(TRIGGER_XPAD_PAD_DOWN) == 1)
        {
            if((kbType == IP_KEYPAD && cursorposY == 2 && cursorposX == 0) || cursorposY == 3)         //Already at the last line
            {
                cursorposY = 0;         //Roll to top
            }
            else
            {
                cursorposY += 1;
            }
            refresh = true;
        }

        if (risefall_xpad_BUTTON(TRIGGER_XPAD_PAD_LEFT) == 1)
        {
            if(kbType == IP_KEYPAD && cursorposY == 3 && cursorposX == 1)
            {
            	cursorposX = rowLength - 1;
            }
            else if(cursorposX == 0)         //Already at the first column
            {
                cursorposX = rowLength - 1;         //Roll to last
            }
            else
            {
                cursorposX -= 1;
            }
            refresh = true;
        }

        if (risefall_xpad_BUTTON(TRIGGER_XPAD_PAD_RIGHT) == 1)
        {
            if(kbType == IP_KEYPAD && cursorposY == 3 && cursorposX == (rowLength - 1))
            {
            	cursorposX = 1;
            }
            else if(cursorposX == (rowLength - 1))         //Already at the last column
            {
                cursorposX = 0;         //Roll to first
            }
            else
            {
                cursorposX += 1;
            }
            refresh = true;
        }
    
        if(exit)
        {
            BootVideoClearScreen(&jpegBackdrop, 0, 0xffff);
            VIDEO_CURSOR_POSX=0;
            VIDEO_CURSOR_POSY=0;
            return;
        }
    }
    return;    //Keep compiler happy.
}
