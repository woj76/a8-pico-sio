; This is a modified version of tiny xdos from the Numen demo
; found at https://github.com/pfusik/numen/blob/master/dos.asx
; See the original repository for credits.

SECTOR_SIZE = 128 ; SD/ED

#define BASIC_CHECK

coldst	=	$244
runad	=	$2e0
initad	=	$2e2
dcomnd	=	$302
dbuflo	=	$304
dbufhi	=	$305
daux1	=	$30a
daux2	=	$30b
dskinv	=	$e453
skctl	=	$d20f
vcount	=	$d40b

boot_flag = 	$09
load_ptr =	$44
load_end =	$46

boot_start =	$700

buffer	=	$800

	*=boot_start

buffer_ofs

;	.byte 'F',2
	.byte 0,boot_blocks+1 ; Note! this does not work when the loader size
				; divided evenly over 128 blocks
	.word boot_start : reloc01 = *-1
;	.word $e477		;?!?!? Relevant when using CASINI only
	.word boot_init : reloc24 = *-1

boot_init
#ifdef BASIC_CHECK
	jsr basic_check : reloc25 = *-1
#endif
	lda #0 : sta coldst
	sta buffer+SECTOR_SIZE-1 : reloc02 = *-1
	sta buffer_ofs : reloc03 = *-1
	ldy #$7F
clear_zp_loop:
	sta $80,y : dey : bpl clear_zp_loop
	lda #$01 : sta buffer+SECTOR_SIZE-3 : reloc04 = *-1
	; sta boot_flag
	lda #$71 : sta buffer+SECTOR_SIZE-2 : reloc05 = *-1

load_1
	jsr read : reloc06 = *-1 : bmi load_run : sta load_ptr
	jsr read : reloc07 = *-1 : bmi load_run : sta load_ptr+1
	and load_ptr : cmp #$ff : beq load_1
	jsr read : reloc08 = *-1 : bmi load_run : sta load_end
	jsr read : reloc09 = *-1 : bmi load_run : sta load_end+1
	lda #0 : runad_ready = *-1 : bne load_1_1
	inc runad_ready : reloc27 = *-1
	lda load_ptr : sta runad : lda load_ptr+1 : sta runad+1
load_1_1
	lda #<read_ret : sta initad
	lda #>read_ret : reloc10 = *-1 : sta initad+1
load_2
	jsr read : reloc11 = *-1 : bmi load_run
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
	lda #$03 : sta skctl
	jmp (initad)
load_run
	lda #$03 : sta skctl
	jmp (runad)

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

#ifdef BASIC_CHECK
ramtop = $6a
ramsiz = $2e4
portb = $d301
basicf = $3f8
edev_vecs = $e400

basic_check
	lda #$C0
	cmp ramtop
	beq basic_check_ret
	sta ramtop : sta ramsiz
	lda portb : ora #$02 : sta portb
	lda #1 : sta basicf
	ldx #2
	jsr editor : reloc26 = *-1
	ldx #0
editor
	lda edev_vecs+1,x
	pha
	lda edev_vecs,x
	pha
basic_check_ret
	rts
#endif
boot_end

boot_len = boot_end - boot_start
boot_blocks = boot_len / 128

	.dsb	((boot_blocks+1)*128 - boot_len),$00
