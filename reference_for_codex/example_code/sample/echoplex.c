#include <conio.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <6502.h>

unsigned char * gtia_bgcol = (unsigned char *) 0xd01a;
unsigned char * pokeybase = (unsigned char *) 0xd200;
unsigned char * sample = (unsigned char *) 0xd280;
unsigned char * configbase = (unsigned char *) 0xd210;

unsigned char nextsample[4];
unsigned char activesample = 0;
unsigned char iter = 0;
typedef void (*irqhandler)();
irqhandler oldirq;
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
		sample[10] = nextsample[0];  // startH
		sample[13] = 223-iter; //perL
		if (nextsample[0]==0x0)
		{
			activesample = activesample&0xfe;
			sample[16] = activesample; // dma off
			//sample[17] = 0; // irq off
			iter = iter+1;
		}
		else if (nextsample[0]==0xa0)
		{
			nextsample[0] = 0;
		}
		else
		{
			nextsample[0] = nextsample[0] + 0x20;
		}
	}
	if (sample[18]&2)
	{
		sample[8] = 2;
		sample[18] = 0xfd;
		sample[10] = nextsample[1];  // startH
		sample[13] = 223-iter; //perL
		if (nextsample[1]==0x0)
		{
			activesample = activesample&0xfd;
			sample[16] = activesample; // dma off
			//sample[17] = 0; // irq off
			iter = iter+1;
		}
		else if (nextsample[1]==0xa0)
		{
			nextsample[1] = 0;
		}
		else
		{
			nextsample[1] = nextsample[1] + 0x20;
		}
	}
	if (sample[18]&4)
	{
		sample[8] = 3;
		sample[18] = 0xfb;
		sample[10] = nextsample[2];  // startH
		sample[13] = 223-iter; //perL
		if (nextsample[2]==0x0)
		{
			activesample = activesample&0xfb;
			sample[16] = activesample; // dma off
			//sample[17] = 0; // irq off
			iter = iter+1;
		}
		else if (nextsample[2]==0xa0)
		{
			nextsample[2] = 0;
		}
		else
		{
			nextsample[2] = nextsample[2] + 0x20;
		}
	}
	if (sample[18]&8)
	{
		sample[8] = 4;
		sample[18] = 0xf7;
		sample[10] = nextsample[3];  // startH
		sample[13] = 223-iter; //perL
		if (nextsample[3]==0x0)
		{
			activesample = activesample&0xf7;
			sample[16] = activesample; // dma off
			//sample[17] = 0; // irq off
			iter = iter+1;
		}
		else if (nextsample[3]==0xa0)
		{
			nextsample[3] = 0;
		}
		else
		{
			nextsample[3] = nextsample[3] + 0x20;
		}
	}

	asm("pla");
	asm("tax");
	asm("pla");

	//asm("pha");
	//asm("jmp %v",oldirq);

	//oldirq();
	//asm("pha");
	//asm("rti");
	asm("jmp (%v)",oldirq);
}

void playEchoPlex()
{
    	FILE * input = 0;
	char * buf = malloc(256);
	char * stack = malloc(256);
	unsigned char c;
	int channel=1;

	unsigned int rep;
	int i;

	oldirq = OS.vimirq;

	OS.vimirq = &handle_sample_irq;

	printf("Loading echoplex.ima from any drive (1-4)\n");
    	for (i=0;i!=4;++i)
	{
		char buffer[] = "d0:echoplex.ima";
		buffer[1] = '1'+i;
		input = fopen(buffer,"r");
		if (input)
			break;
	}
	if (i==4)
	{
		gotoxy(0,3);
		printf("Cannot find echoplex.ima!\n");
		return;
	}

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

	printf("Press keys to play sample!\n");

	while (1)
	{
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

		iter = c;

		sample[8] = channel;
		sample[9] = 0x0;  // startL
		sample[11] = 0xFF; // lenL
		sample[12] = 0x3F; // lenH
		sample[13] = 223; //perL
		sample[14] = 0x0; //perH
		sample[15] = 0x3f; //vol 

		sample[10] = 0x0;  // startH
		nextsample[channel-1] = 0x20;
		SEI();
		activesample = activesample|(1<<(channel-1));
		//sample[19] = 0x00;//activesample; //adpcm 
		sample[19] = activesample; //adpcm 
		sample[17] = activesample; // irq on
		sample[16] = activesample; // dma on
		CLI();

		printf("key:%c ch:%d act:%d\n",c, channel,activesample); // TODO: need to handle pokey irq to get keypress!

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
	printf("Pokeymax test: Sample\n");
	
	playEchoPlex();

	while (!kbhit());
	cgetc();

	stopSample();

	return 0;
}

