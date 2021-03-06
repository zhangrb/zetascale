//----------------------------------------------------------------------------
// ZetaScale
// Copyright (c) 2016, SanDisk Corp. and/or all its affiliates.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License version 2.1 as published by the Free
// Software Foundation;
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License v2.1 for more details.
//
// A copy of the GNU Lesser General Public License v2.1 is provided with this package and
// can also be found at: http://opensource.org/licenses/LGPL-2.1
// You should have received a copy of the GNU Lesser General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 59 Temple
// Place, Suite 330, Boston, MA 02111-1307 USA.
//----------------------------------------------------------------------------


/*
 * POT element
 *
 * Currently equivalent to mcd_rec_flash_object_t, but likely to diverge.
 */
struct mcdpotelemstructure {
	uint16_t	osyndrome,	// 16-bit syndrome
			tombstone:1,	// 1=entry is a tombstone
			deleted:1,	// 1=marked for delete-in-future
			reserved:2,	// reserved
			blocks:12;	// number of 512-byte blocks occupied
	uint32_t	obucket;	// hash bucket
	uint64_t	cntr_id:16,	// container id
			seqno:48;	// sequence number
};

/*
 * POT bitmap on flash
 *
 * Also serves as the in-memory live structure.  Aligned for direct
 * flash I/O.
 */
struct mcdpotbitmapstructure {
	uint32_t	eye_catcher;
	uint64_t	checksum,
		nbit;
	uint8_t	bits[];
};

/*
 * slab bitmap on flash
 *
 * Also serves as the in-memory live structure.  Aligned for direct
 * flash I/O.
 */
struct mcdslabbitmapstructure {
	uint32_t	eye_catcher;
	uint64_t	checksum,
		nbit;
	uint8_t	bits[];
};

typedef struct mcdpotbitmapstructure	potbm_t;
typedef struct mcdslabbitmapstructure	slabbm_t;

enum {
	MCD_REC_POTBM_EYE_CATCHER = 0x42544F50,		// "POTB"
	MCD_REC_SLABBM_EYE_CATCHER = 0x42424C53		// "SLBB"
};


#define ZS_BYTES_PER_STORM_KEY 		512
#define	bits_per_byte			8uL
#define	bitbase( a)	((a) / bits_per_byte)
#define	bitmask( a)	(1 << (a)%bits_per_byte)

#define bytes_per_second                (1uL << 31)
#define bytes_per_segment               (1uL << 25)
#define bytes_per_pot_element           (1uL << 4)
#define bytes_per_log_record            (1uL << 6)
#define device_blocks_per_storm_object  (bytes_per_storm_object / bytes_per_device_block)
#define device_blocks_per_segment       (bytes_per_segment / ZS_DEFAULT_BLOCK_SIZE)
#define pot_elements_per_page           device_blocks_per_segment
#define bytes_per_page                  (pot_elements_per_page * bytes_per_pot_element)
#define leaf_occupancy_pct              75
#define regobj_scale_pct                120

ulong		mcd_rec2_standard_slab_segments( ulong),
		mcd_rec2_log_size( ulong),
		mcd_rec2_potbitmap_size( ulong),
		mcd_rec2_slabbitmap_size( ulong);
bool		mcd_rec2_init( ulong),
		mcd_rec2_potcache_init( mcd_osd_shard_t *, osd_state_t *),
		mcd_rec2_potcache_save( mcd_osd_shard_t *, void *),
		mcd_rec2_potbitmap_load( mcd_osd_shard_t *, void *),
		mcd_rec2_potbitmap_save( mcd_osd_shard_t *, void *),
		mcd_rec2_potbitmap_query( mcd_osd_shard_t *, uint),
		mcd_rec2_slabbitmap_load( mcd_osd_shard_t *, void *),
		mcd_rec2_slabbitmap_save( mcd_osd_shard_t *, void *);
void		mcd_rec2_potbitmap_set( mcd_osd_shard_t *, uint),
		mcd_rec2_shutdown( mcd_osd_shard_t *);
bool		check_storm_mode( ),
		get_rawobjsz( uint64_t *);
uint64_t	get_rawobj_size();
int		get_rawobjratio( );
uint64_t	get_regobj_storm_mode( );

extern int rawobjratio;
