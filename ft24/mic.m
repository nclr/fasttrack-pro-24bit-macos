/* Microphone (TCC) permission check. Without it, macOS delivers silence from every
 * input device, BlackHole included. Returns 1 when granted. */
#import <AVFoundation/AVFoundation.h>

int mic_permission(void) {
    AVAuthorizationStatus s = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (s == AVAuthorizationStatusAuthorized) return 1;
    if (s != AVAuthorizationStatusNotDetermined) return 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block BOOL granted = NO;
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL ok) {
                                 granted = ok;
                                 dispatch_semaphore_signal(sem);
                             }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC));
    return granted ? 1 : 0;
}
