#include <conio.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <6502.h>

#ifdef C64
unsigned char * configbase = (unsigned char *) 0xd4f0;
unsigned char * configenablebase = (unsigned char *) 0xd4f0;
#else
unsigned char * configbase = (unsigned char *) 0xd210;
unsigned char * configenablebase = (unsigned char *) 0xd200;
#endif

void configMode(unsigned char onoff)
{
	if (onoff)
		configenablebase[0xc] = 0x3f;
	else
		configenablebase[0xc] = 0x0;

}

int main(void)
{
	unsigned char i=0;
	clrscr();
	printf("Pokeymax test: Detect\n");

	i = configbase[0xc];
	if (i==1)
	{
		printf("Pokeymax detected\n");
		configMode(1);
		i = configbase[0x1];
		if (i&0x40)
			printf("  Flash support\n");
		if (i&0x20)
			printf("  Sample support\n");
		if (i&0x10)
			printf("  Covox support\n");
		if (i&0x8)
			printf("  PSG support\n");
		if (i&0x4)
			printf("  SID support\n");
		i = i&3;
		if (i==0)
			printf("  Mono pokey\n");
		else if (i==1)
			printf("  Stereo pokey\n");
		else if (i==2 || i==3)
			printf("  Quad pokey\n"); // 3 should be reserved for 8, but there is a bug, so some quads show 3 for now...
		printf("Core: ");
		for (i=0;i!=8;++i)
		{
			configbase[4] = i;
			printf("%c",configbase[4]);
		}
		printf("\n");

		configMode(0);
	}
	else
	{
		printf("Pokeymax not found\n");
	}
	
	return 0;
}
	
