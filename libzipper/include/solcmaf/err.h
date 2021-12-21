//
//  err.h
//  solCMAF
//
//  Created by Hyungpyo Oh on 16/11/2018.
//  Copyright © 2018 Hyungpyo Oh. All rights reserved.
//

#ifndef err_h
#define err_h

enum {

    error_success = 0,
    error_alloc = 1,
    error_thread = 2,
    error_param = 3,
    error_support = 4,
    error_dns = 5,
    
    error_socOpen = 6,
    error_socConnect = 7,
    
    error_manifest = 8,
    error_chunk = 9,
    error_eof = 10,
    error_chain = 11,
    error_skip = 12,
    error_format = 13,
    error_target = 14,
};

#endif /* err_h */
