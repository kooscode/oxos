#include "ProgressBar.h"
#include "boot.h"

void DisplayProgressBar(int currentVal, int maxVal, unsigned long color) {
    int x,y,l,w,h,m;
    unsigned int *fb=(unsigned int*)FB_START;

    if(maxVal<2){
        return;
    }

    w=vmode.width;
    h=vmode.height;
    l=w-100;
    y=h-100;
    m=((w-155)>>2)*currentVal/maxVal;
    m*=4;
    m+=50;
    for(x=50;x<l;x++){
        // Top white line
        fb[y*w+x]=0xffffffff;
        fb[(y+1)*w+x]=0xffffffff;


        if(x>55 && x<m){
            int z;
            for(z=5;z<45;z++){
                fb[(y+z)*w+x]=color;
            }
        }


        // Bottom white line
        fb[(y+50)*w+x]=0xffffffff;
        fb[(y+51)*w+x]=0xffffffff;
    }

    // Draw vertical white lines of the rectangle
    for(;y<h-50;y++){
        // left
        fb[y*w+51]=0xffffffff;
        fb[y*w+50]=0xffffffff;
        // right
        fb[y*w+l-1]=0xffffffff;
        fb[y*w+l-2]=0xffffffff;
    }
}

