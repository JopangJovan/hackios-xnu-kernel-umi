#ifndef UMI_VOICE_SYSTEM_H
#define UMI_VOICE_SYSTEM_H

#import <UIKit/UIKit.h>
#include <sys/umi_sdx55_voice.h>

int umi_voice_submit(unsigned action, NSString *number);
int umi_voice_read_status(struct umi_voice_snapshot *status);
void umi_voice_diagnostic(const char *message);
BOOL umi_voice_start_system_call(NSString *number, void (^fallback)(BOOL alreadyDialed));

#endif
