// Copyright (c) 2026 Ihor Lukianow
// SPDX-License-Identifier: MIT OR Apache-2.0

#ifndef BLARIS_CONFIG_H
#define BLARIS_CONFIG_H

/*
 * Compression level presets (1..9), or 0 for user-defined custom config.
 */
#ifdef BLARIS_LEVEL
#    if BLARIS_LEVEL < 0 || BLARIS_LEVEL > 9
#        error "BLARIS_LEVEL must be between 0 and 9"
#    elif BLARIS_LEVEL == 0
/* User-defined custom config: leave all flags to caller or defaults. */
#    elif BLARIS_LEVEL == 1
#        ifndef BLARIS_DISTANCE_ALIGN_LOG
#            define BLARIS_DISTANCE_ALIGN_LOG 2
#        endif
#        ifndef BLARIS_CHECKPOINTS_SPREAD_LOG
#            define BLARIS_CHECKPOINTS_SPREAD_LOG 6
#        endif
#        ifndef BLARIS_HOP_PENALTY
#            define BLARIS_HOP_PENALTY 16
#        endif
#        ifndef BLARIS_MAX_HOPS
#            define BLARIS_MAX_HOPS 8
#        endif
#        ifndef BLARIS_CHECKPOINT_MAX_PENALTY
#            define BLARIS_CHECKPOINT_MAX_PENALTY 384
#        endif
#        ifndef BLARIS_MATCH_LENGTH_PENALTY
#            define BLARIS_MATCH_LENGTH_PENALTY 64
#        endif
#    elif BLARIS_LEVEL == 2
#        ifndef BLARIS_DISTANCE_ALIGN_LOG
#            define BLARIS_DISTANCE_ALIGN_LOG 2
#        endif
#        ifndef BLARIS_CHECKPOINTS_SPREAD_LOG
#            define BLARIS_CHECKPOINTS_SPREAD_LOG 8
#        endif
#        ifndef BLARIS_HOP_PENALTY
#            define BLARIS_HOP_PENALTY 16
#        endif
#        ifndef BLARIS_MAX_HOPS
#            define BLARIS_MAX_HOPS 8
#        endif
#        ifndef BLARIS_CHECKPOINT_MAX_PENALTY
#            define BLARIS_CHECKPOINT_MAX_PENALTY 384
#        endif
#        ifndef BLARIS_MATCH_LENGTH_PENALTY
#            define BLARIS_MATCH_LENGTH_PENALTY 64
#        endif
#    elif BLARIS_LEVEL == 3
#        ifndef BLARIS_CHECKPOINTS_SPREAD_LOG
#            define BLARIS_CHECKPOINTS_SPREAD_LOG 8
#        endif
#        ifndef BLARIS_HOP_PENALTY
#            define BLARIS_HOP_PENALTY 16
#        endif
#        ifndef BLARIS_MAX_HOPS
#            define BLARIS_MAX_HOPS 8
#        endif
#        ifndef BLARIS_CHECKPOINT_MAX_PENALTY
#            define BLARIS_CHECKPOINT_MAX_PENALTY 384
#        endif
#        ifndef BLARIS_MATCH_LENGTH_PENALTY
#            define BLARIS_MATCH_LENGTH_PENALTY 64
#        endif
#    elif BLARIS_LEVEL == 4
#        ifndef BLARIS_CHECKPOINTS_SPREAD_LOG
#            define BLARIS_CHECKPOINTS_SPREAD_LOG 8
#        endif
#        ifndef BLARIS_HOP_PENALTY
#            define BLARIS_HOP_PENALTY 16
#        endif
#        ifndef BLARIS_MAX_HOPS
#            define BLARIS_MAX_HOPS 8
#        endif
#        ifndef BLARIS_CHECKPOINT_MAX_PENALTY
#            define BLARIS_CHECKPOINT_MAX_PENALTY 384
#        endif
#        ifndef BLARIS_MATCH_LENGTH_PENALTY
#            define BLARIS_MATCH_LENGTH_PENALTY 16
#        endif
#    elif BLARIS_LEVEL == 5
#        ifndef BLARIS_CHECKPOINTS_SPREAD_LOG
#            define BLARIS_CHECKPOINTS_SPREAD_LOG 8
#        endif
#        ifndef BLARIS_HOP_PENALTY
#            define BLARIS_HOP_PENALTY 16
#        endif
#        ifndef BLARIS_MAX_HOPS
#            define BLARIS_MAX_HOPS 8
#        endif
#        ifndef BLARIS_CHECKPOINT_MAX_PENALTY
#            define BLARIS_CHECKPOINT_MAX_PENALTY 0
#        endif
#        ifndef BLARIS_MATCH_LENGTH_PENALTY
#            define BLARIS_MATCH_LENGTH_PENALTY 16
#        endif
#    elif BLARIS_LEVEL == 6
#        ifndef BLARIS_CHECKPOINTS_SPREAD_LOG
#            define BLARIS_CHECKPOINTS_SPREAD_LOG 8
#        endif
#        ifndef BLARIS_HOP_PENALTY
#            define BLARIS_HOP_PENALTY 4
#        endif
#        ifndef BLARIS_MAX_HOPS
#            define BLARIS_MAX_HOPS 8
#        endif
#        ifndef BLARIS_CHECKPOINT_MAX_PENALTY
#            define BLARIS_CHECKPOINT_MAX_PENALTY 0
#        endif
#        ifndef BLARIS_MATCH_LENGTH_PENALTY
#            define BLARIS_MATCH_LENGTH_PENALTY 16
#        endif
#    elif BLARIS_LEVEL == 7
#        ifndef BLARIS_CHECKPOINTS_SPREAD_LOG
#            define BLARIS_CHECKPOINTS_SPREAD_LOG 10
#        endif
#        ifndef BLARIS_HOP_PENALTY
#            define BLARIS_HOP_PENALTY 1
#        endif
#        ifndef BLARIS_MAX_HOPS
#            define BLARIS_MAX_HOPS 32
#        endif
#        ifndef BLARIS_CHECKPOINT_MAX_PENALTY
#            define BLARIS_CHECKPOINT_MAX_PENALTY 0
#        endif
#        ifndef BLARIS_MATCH_LENGTH_PENALTY
#            define BLARIS_MATCH_LENGTH_PENALTY 8
#        endif
#    elif BLARIS_LEVEL == 8
#        ifndef BLARIS_CHECKPOINTS_SPREAD_LOG
#            define BLARIS_CHECKPOINTS_SPREAD_LOG 10
#        endif
#        ifndef BLARIS_FLATTEN_CHAINS
#            define BLARIS_FLATTEN_CHAINS 0
#        endif
#    elif BLARIS_LEVEL == 9
#        ifndef BLARIS_CHECKPOINTS_SPREAD_LOG
#            define BLARIS_CHECKPOINTS_SPREAD_LOG 0
#        endif
#        ifndef BLARIS_FLATTEN_CHAINS
#            define BLARIS_FLATTEN_CHAINS 0
#        endif
#    endif
#endif

#endif
