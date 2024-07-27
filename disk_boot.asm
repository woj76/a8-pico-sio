SECTOR_SIZE = 128 ; SD/ED

runad	=	$2e0
initad	=	$2e2
dcomnd	=	$302
dbuflo	=	$304
dbufhi	=	$305
daux1	=	$30a
daux2	=	$30b
dskinv	=	$e453

load_ptr =	$44
load_end =	$46

boot_start =	$700

buffer	=	boot_start-$80

	*=boot_start

buffer_ofs

	.byte 'F',2
	.word buffer_ofs : reloc01 = *-1
	.word $e477		;?!?!? Relevant when using CASINI only

	lda #0 : sta buffer+SECTOR_SIZE-1 : reloc02 = *-1 : sta buffer_ofs : reloc03 = *-1
	lda #$01 : sta buffer+SECTOR_SIZE-3 : reloc04 = *-1
	lda #$71 : sta buffer+SECTOR_SIZE-2 : reloc05 = *-1

load_1
	jsr read : reloc06 = *-1 : bmi load_run : sta load_ptr
	jsr read : reloc07 = *-1 : bmi load_error : sta load_ptr+1
	cmp #$ff : bcs load_1
	jsr read : reloc08 = *-1 : bmi load_error : sta load_end
	jsr read : reloc09 = *-1 : bmi load_error : sta load_end+1
	lda #<read_ret : sta initad
	lda #>read_ret : reloc10 = *-1 : sta initad+1
load_2
	jsr read : reloc11 = *-1 : bmi load_error
	ldy #$00
	sta (load_ptr),y
	ldy load_ptr
	lda load_ptr+1
	inc load_ptr
	bne *+4
	inc load_ptr+1
	cpy load_end
	sbc load_end+1
	bcc load_2
	lda #>(load_1-1) : reloc12 = *-1 : pha
	lda #<(load_1-1) : pha
	jmp (initad)
load_run
	jmp (runad)
load_error
	sec
	rts

sio_next
	lda buffer+SECTOR_SIZE-3 : reloc13 = *-1 : ldy buffer+SECTOR_SIZE-2 : reloc14 = *-1
	bne sio_sector
	cmp #0
	beq eof
sio_sector
	sty daux1 : sta daux2
	lda #0 : sta buffer+SECTOR_SIZE-3 : reloc15 = *-1 : sta buffer+SECTOR_SIZE-2 : reloc16 = *-1
sio_command
	stx dcomnd
	lda #>buffer : reloc17 = *-1 : sta dbufhi
	lda #<buffer : sta dbuflo
	jmp dskinv
eof
	ldy #136
	rts

read
	ldy buffer_ofs : reloc18 = *-1 : cpy buffer+SECTOR_SIZE-1 : reloc19 = *-1 : bcc read_get
	ldx #'R'
	jsr sio_next : reloc20 = *-1
	bmi read_ret
	ldy buffer+SECTOR_SIZE-1 : reloc21 = *-1
	beq eof
	ldy #0
read_get
	lda buffer,y : reloc22 = *-1
	iny : sty buffer_ofs : reloc23 = *-1
success
	ldy #1
read_ret
	rts

all_end = buffer_ofs+$100
	.dsb	(all_end-*),$00
