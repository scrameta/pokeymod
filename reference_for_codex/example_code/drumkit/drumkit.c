#include <conio.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <6502.h>

unsigned char * sample = (unsigned char *) 0xd280;

unsigned char activesample = 0;
typedef void (*irqhandler)();
irqhandler oldirq;
int finish[4];
void handle_sample_irq() 
{
	asm("pha");
	asm("txa");
	asm("pha");

	*(unsigned char*)0xD01A = *(unsigned char*)0xD20A;
	if (sample[18]&1)
	{
		sample[8] = 1;
		sample[18] = 0xfe;

		if (finish[0])
		{
			activesample = activesample&0xfe;
			sample[16] = activesample; // dma off
		}
		finish[0] = 1;
	}
	if (sample[18]&2)
	{
		sample[8] = 2;
		sample[18] = 0xfd;
		if (finish[1])
		{
			activesample = activesample&0xfd;
			sample[16] = activesample; // dma off
		}
		finish[1] = 1;
	}
	if (sample[18]&4)
	{
		sample[8] = 3;
		sample[18] = 0xfb;
		if (finish[2])
		{
			activesample = activesample&0xfb;
			sample[16] = activesample; // dma off
		}
		finish[2] = 1;
	}
	if (sample[18]&8)
	{
		sample[8] = 4;
		sample[18] = 0xf7;
		if (finish[3])
		{
			activesample = activesample&0xf7;
			sample[16] = activesample; // dma off
		}
		finish[3] = 1;
	}

	asm("pla");
	asm("tax");
	asm("pla");

	asm("jmp (%v)",oldirq);
}

	unsigned int locs[] = {0u,280u,5707u,10519u,11824u,15709u,16021u,16623u,17112u,18306u,19054u,20408u,20668u,22950u,23144u,25377u,25672u,26424u,28059u,28778u,33293u,36806u,39966u,42959u};

	//23

void playDrums()
{
    	FILE * input = 0;
	char * buf = malloc(256);
	char * stack = malloc(256);
	unsigned char c;
	int channel=1;

	unsigned int rep;
	unsigned int a=0;
	int i;

	gotoxy(0,2);
	cprintf("Loading drums.ima from any drive (1-4)");
    	for (i=0;i!=4;++i)
	{
		char buffer[] = "d0:drums.ima";
		buffer[1] = '1'+i;
		input = fopen(buffer,"r");
		if (input)
			break;
	}
	if (i==4)
	{
		gotoxy(0,3);
		cprintf("Cannot find drums.ima!");
		return;
	}

	oldirq = OS.vimirq;

	OS.vimirq = &handle_sample_irq;

	sample[4] = 0;
	sample[5] = 0x00;
	for (rep=0;rep!=168;++rep)
	{
		int r = fread(buf,1,256,input);
		unsigned int i;
		for (i=0;i!=r;++i)
		{
			sample[7] = buf[i]; // auto increment
		}
	}
	fclose(input);

	gotoxy(0,3);
	cprintf("Loaded, press any key to play a drum");

	while (1)
	{
		int drum;
		unsigned int start;
		unsigned int len;

		while (!kbhit());
		c = cgetc();
		if (c=='Q') break;
		if (c=='1') 
		{
			channel = 1;
			continue;
		}
		if (c=='2') 
		{
			channel = 2;
			continue;
		}
		if (c=='3') 
		{
			channel = 3;
			continue;
		}
		if (c=='4') 
		{
			channel = 4;
			continue;
		}

		if (c>='a' && c<('a'+23))
		{
			drum = c-'a';
		}
		else
			continue;
		

		start = locs[drum];
		len = (locs[drum+1]-start)<<1;
		sample[8] = channel;
		sample[9] = start&0xff;  // startL
		sample[10] = start>>8;  // startH
		sample[11] = len&0xff; // lenL
		sample[12] = len>>8; // lenH
		sample[13] = ((unsigned int)358)&0xff; //perL
		sample[14] = ((unsigned int)358)>>8; //perH
		sample[15] = 0x3f; //vol 

		SEI();
		activesample = activesample|(1<<(channel-1));
		sample[19] = activesample; //adpcm 
		finish[channel-1] = 0;
		sample[17] = activesample; // irq on
		sample[16] = activesample; // dma on
		CLI();

	//	cprintf("%d:%d:%u:%u\n",channel,drum,start,len); // TODO: need to handle pokey irq to get keypress!

		channel = channel+1;
		if (channel>4) channel = 1;
	}

	OS.vimirq = oldirq;
}

void stopSample()
{
	sample[16] = 0; // dma off
	sample[17] = 0; // irq off
}

int main(void)
{
	unsigned char i=0;
	clrscr();
	cprintf("Pokeymax test: Drum kit");
	
	playDrums();

	while (!kbhit());
	cgetc();

	stopSample();

	return 0;
}
