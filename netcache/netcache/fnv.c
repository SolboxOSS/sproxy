// [North Star One-Sword School]
// - My name is Kanichiro Yoshimura.
//   I'm a new man. Just so you'll know who I am...
//   Saito-sensei.
//   What land are you from?
// - 'Land'?
//   Yes.
//   I was born in Morioka, in Nanbu, Oshu.
//   It's a beautiful place.
//   Please...
//   Away to the south is Mt Hayachine...
//   with Mt Nansho and Mt Azumane to the west.
//   In the north are Mt Iwate and Mt Himekami.
//   Out of the high mountains flows the Nakatsu River...
//   through the castle town into the Kitakami below Sakuranobaba.
//   Ah, it's pretty as a picture!
//   There's nowhere like it in all Japan!
// ...
// - Hijikata-sensei... as you're aware, the circumstances... made the task quite difficult.
//   It caused a chip in the blade of my sword.
//   Could I perhaps ask for... the cost of a new sword?
// - That should do, Your blade doesn't bear its maker's name.
// - You're too kind. My humble thanks!
// - What kind of a samurai is that?
// - He's really something!
// ...
// - Where's it chipped?
// - My sword's as worn down as I am.
// ...
// The Shinsengumi was... the sterile flower of the Shoguns' end.
// /Paragon Kiichi Nakai in the paragon piece-of-art 'The Wolves of Mibu' aka 'WHEN THE LAST SWORD IS DRAWN'/
// As I said on one Japanese forum, Kiichi Nakai deserves an award worth his weight in gold, nah-nah, in DIAMONDS!
uint32_t FNV1A_Hash_Yoshimura(const char *str, uint32_t wrdlen)
{
	const uint32_t PRIME = 709607;
	uint32_t hash32 = 2166136261;
	uint32_t hash32B = 2166136261;
	const char *p = str;
	uint32_t Loop_Counter;
	uint32_t Second_Line_Offset;

	if (wrdlen >= 2 * 2 * sizeof(uint32_t)) {
		Second_Line_Offset = wrdlen - ((wrdlen >> 4) + 1) * (2 * 4);	// ((wrdlen>>1)>>3)
		Loop_Counter = (wrdlen >> 4);
		//if (wrdlen%16) Loop_Counter++;
		Loop_Counter++;
		for (; Loop_Counter; Loop_Counter--, p += 2 * sizeof(uint32_t)) {
			// revision 1:
			//hash32 = (hash32 ^ (_rotl(*(uint32_t *)(p+0),5) ^ *(uint32_t *)(p+4))) * PRIME;        
			//hash32B = (hash32B ^ (_rotl(*(uint32_t *)(p+0+Second_Line_Offset),5) ^ *(uint32_t *)(p+4+Second_Line_Offset))) * PRIME;        
			// revision 2:
			hash32 =
				(hash32 ^
				 (_rotl(*(uint32_t *) (p + 0), 5) ^
				  *(uint32_t *) (p + 0 + Second_Line_Offset))) * PRIME;
			hash32B =
				(hash32B ^
				 (_rotl(*(uint32_t *) (p + 4 + Second_Line_Offset), 5) ^
				  *(uint32_t *) (p + 4))) * PRIME;
		}
	} else {
		// Cases: 0,1,2,3,4,5,6,7,...,15
		if (wrdlen & 2 * sizeof(uint32_t)) {
			hash32 = (hash32 ^ *(uint32_t *) (p + 0)) * PRIME;
			hash32B = (hash32B ^ *(uint32_t *) (p + 4)) * PRIME;
			p += 4 * sizeof(uint16_t);
		}
		// Cases: 0,1,2,3,4,5,6,7
		if (wrdlen & sizeof(uint32_t)) {
			hash32 = (hash32 ^ *(uint16_t *) (p + 0)) * PRIME;
			hash32B = (hash32B ^ *(uint16_t *) (p + 2)) * PRIME;
			p += 2 * sizeof(uint16_t);
		}
		if (wrdlen & sizeof(uint16_t)) {
			hash32 = (hash32 ^ *(uint16_t *) p) * PRIME;
			p += sizeof(uint16_t);
		}
		if (wrdlen & 1)
			hash32 = (hash32 ^ *p) * PRIME;
	}
	hash32 = (hash32 ^ _rotl(hash32B, 5)) * PRIME;
	return hash32 ^ (hash32 >> 16);
}
