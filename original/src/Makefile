all: ext4recover bgrep_demo

ext4recover: ext4recover.c
	$(CC) -Wno-error=return-type -Wno-error=int-conversion -o $@ $< -lcom_err -lext2fs 

bgrep_%: bgrep_%.c
	$(CC) -Wno-error=return-type -Wno-error=int-conversion -o $@ $<

clean:
	rm -f ext4recover bgrep_demo bgrep_non_zero
