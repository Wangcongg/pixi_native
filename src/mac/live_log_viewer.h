#pragma once

#import <Cocoa/Cocoa.h>

#include <sys/stat.h>

// In-app tail viewer for packaged builds. It reads the same runtime.log written
// by persistent_log.h, so showing or closing this window never changes file
// persistence, Xcode console mirroring, or rotation behavior.
@interface MacLiveLogWindowController : NSWindowController <NSWindowDelegate> {
@private
    NSString* _logPath;
    NSTextView* _textView;
    NSTextField* _statusLabel;
    NSButton* _autoScrollButton;
    NSTimer* _pollTimer;
    NSMutableData* _pendingBytes;
    unsigned long long _readOffset;
    ino_t _fileInode;
    BOOL _hasOpenedFile;
    BOOL _discardLeadingFragment;
}
- (instancetype)initWithLogPath:(NSString*)logPath;
- (void)showLiveLogWindow;
- (void)stop;
@end

@implementation MacLiveLogWindowController

static const unsigned long long kMacLiveLogInitialTailBytes = 512 * 1024;
static const NSUInteger kMacLiveLogMaximumCharacters = 2 * 1024 * 1024;
static const NSUInteger kMacLiveLogRetainedCharacters = 1024 * 1024;

- (instancetype)initWithLogPath:(NSString*)logPath {
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 960, 640)
                  styleMask:(NSWindowStyleMaskTitled |
                             NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self = [super initWithWindow:window];
    if (!self) return nil;

    _logPath = [logPath copy];
    _pendingBytes = [NSMutableData data];
    window.title = @"Pixi Native — 实时日志";
    window.minSize = NSMakeSize(680, 360);
    window.releasedWhenClosed = NO;
    window.delegate = self;
    [window center];
    [self buildContent];
    return self;
}

- (void)buildContent {
    NSView* content = self.window.contentView;
    content.wantsLayer = YES;
    content.layer.backgroundColor = [NSColor colorWithSRGBRed:0.055
                                                        green:0.062
                                                         blue:0.078
                                                        alpha:1.0].CGColor;

    NSView* header = [[NSView alloc] initWithFrame:NSZeroRect];
    header.translatesAutoresizingMaskIntoConstraints = NO;
    header.wantsLayer = YES;
    header.layer.backgroundColor = [NSColor colorWithSRGBRed:0.105
                                                       green:0.116
                                                        blue:0.145
                                                       alpha:1.0].CGColor;
    [content addSubview:header];

    _statusLabel = [NSTextField labelWithString:@"● 等待 runtime.log…"];
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _statusLabel.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    _statusLabel.textColor = NSColor.systemOrangeColor;
    [header addSubview:_statusLabel];

    NSTextField* hint = [NSTextField labelWithString:@"错误为红色 · 警告为橙色"];
    hint.translatesAutoresizingMaskIntoConstraints = NO;
    hint.font = [NSFont systemFontOfSize:12];
    hint.textColor = [NSColor colorWithWhite:0.65 alpha:1.0];
    [header addSubview:hint];

    _autoScrollButton = [NSButton checkboxWithTitle:@"自动滚动"
                                             target:nil
                                             action:nil];
    _autoScrollButton.translatesAutoresizingMaskIntoConstraints = NO;
    _autoScrollButton.state = NSControlStateValueOn;
    _autoScrollButton.contentTintColor = NSColor.whiteColor;
    [header addSubview:_autoScrollButton];

    NSButton* clear = [NSButton buttonWithTitle:@"清空显示"
                                         target:self
                                         action:@selector(clearDisplay:)];
    clear.translatesAutoresizingMaskIntoConstraints = NO;
    clear.bezelStyle = NSBezelStyleRounded;
    [header addSubview:clear];

    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = NO;
    scroll.autohidesScrollers = YES;
    scroll.borderType = NSNoBorder;
    scroll.drawsBackground = NO;
    [content addSubview:scroll];

    _textView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 960, 580)];
    _textView.editable = NO;
    _textView.selectable = YES;
    _textView.richText = YES;
    _textView.drawsBackground = YES;
    _textView.backgroundColor = [NSColor colorWithSRGBRed:0.055
                                                    green:0.062
                                                     blue:0.078
                                                    alpha:1.0];
    _textView.textContainerInset = NSMakeSize(12, 10);
    _textView.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    _textView.textColor = [NSColor colorWithWhite:0.84 alpha:1.0];
    _textView.minSize = NSMakeSize(0, 0);
    _textView.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    _textView.verticallyResizable = YES;
    _textView.horizontallyResizable = NO;
    _textView.autoresizingMask = NSViewWidthSizable;
    _textView.textContainer.widthTracksTextView = YES;
    _textView.textContainer.containerSize = NSMakeSize(scroll.contentSize.width, CGFLOAT_MAX);
    scroll.documentView = _textView;

    [NSLayoutConstraint activateConstraints:@[
        [header.topAnchor constraintEqualToAnchor:content.topAnchor],
        [header.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [header.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [header.heightAnchor constraintEqualToConstant:48],

        [_statusLabel.leadingAnchor constraintEqualToAnchor:header.leadingAnchor constant:14],
        [_statusLabel.centerYAnchor constraintEqualToAnchor:header.centerYAnchor],
        [hint.leadingAnchor constraintEqualToAnchor:_statusLabel.trailingAnchor constant:14],
        [hint.centerYAnchor constraintEqualToAnchor:header.centerYAnchor],

        [clear.trailingAnchor constraintEqualToAnchor:header.trailingAnchor constant:-12],
        [clear.centerYAnchor constraintEqualToAnchor:header.centerYAnchor],
        [_autoScrollButton.trailingAnchor constraintEqualToAnchor:clear.leadingAnchor constant:-14],
        [_autoScrollButton.centerYAnchor constraintEqualToAnchor:header.centerYAnchor],

        [scroll.topAnchor constraintEqualToAnchor:header.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];
}

- (void)showLiveLogWindow {
    [self showWindow:nil];
    [self.window makeKeyAndOrderFront:nil];
    if (!_pollTimer) {
        [self pollLog:nil];
        _pollTimer = [NSTimer timerWithTimeInterval:0.2
                                            target:self
                                          selector:@selector(pollLog:)
                                          userInfo:nil
                                           repeats:YES];
        _pollTimer.tolerance = 0.04;
        [NSRunLoop.mainRunLoop addTimer:_pollTimer forMode:NSRunLoopCommonModes];
    }
}

- (void)stop {
    [_pollTimer invalidate];
    _pollTimer = nil;
}

- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    [self stop];
}

- (void)clearDisplay:(id)sender {
    (void)sender;
    [_textView.textStorage setAttributedString:
        [[NSAttributedString alloc] initWithString:@""]];
}

- (void)setWaitingStatus:(NSString*)text {
    _statusLabel.stringValue = text;
    _statusLabel.textColor = NSColor.systemOrangeColor;
}

- (void)setLiveStatus {
    _statusLabel.stringValue = @"● 实时读取 runtime.log";
    _statusLabel.textColor = NSColor.systemGreenColor;
}

- (void)pollLog:(NSTimer*)timer {
    (void)timer;
    if (!_logPath.length) return;

    struct stat fileInfo{};
    if (::stat(_logPath.fileSystemRepresentation, &fileInfo) != 0) {
        [self setWaitingStatus:@"● 等待 runtime.log…"];
        return;
    }

    const BOOL isNewFile = !_hasOpenedFile || _fileInode != fileInfo.st_ino;
    if (isNewFile) {
        if (_hasOpenedFile) {
            [self appendSpecialLine:@"──────── runtime.log 已轮转，继续读取新文件 ────────\n"];
        }
        _hasOpenedFile = YES;
        _fileInode = fileInfo.st_ino;
        [_pendingBytes setLength:0];
        if (_readOffset == 0 && fileInfo.st_size > (off_t)kMacLiveLogInitialTailBytes) {
            _readOffset = (unsigned long long)fileInfo.st_size - kMacLiveLogInitialTailBytes;
            _discardLeadingFragment = YES;
            [self appendSpecialLine:@"──────── 已载入 runtime.log 最近 512 KB ────────\n"];
        } else {
            _readOffset = 0;
            _discardLeadingFragment = NO;
        }
    } else if ((unsigned long long)fileInfo.st_size < _readOffset) {
        // Covers an explicit truncate even when the filesystem reuses the inode.
        _readOffset = 0;
        [_pendingBytes setLength:0];
        _discardLeadingFragment = NO;
        [self appendSpecialLine:@"──────── runtime.log 已清空，重新开始读取 ────────\n"];
    }

    if ((unsigned long long)fileInfo.st_size == _readOffset) {
        [self setLiveStatus];
        return;
    }

    NSFileHandle* handle = [NSFileHandle fileHandleForReadingAtPath:_logPath];
    if (!handle) {
        [self setWaitingStatus:@"● runtime.log 暂时无法读取"];
        return;
    }
    NSData* appended = nil;
    @try {
        [handle seekToFileOffset:_readOffset];
        appended = [handle readDataToEndOfFile];
    } @catch (NSException* exception) {
        (void)exception;
        [self setWaitingStatus:@"● runtime.log 读取失败，正在重试…"];
    }
    [handle closeFile];
    if (!appended.length) {
        [self setLiveStatus];
        return;
    }
    _readOffset += appended.length;
    [_pendingBytes appendData:appended];
    [self consumeCompleteLines];
    [self setLiveStatus];
}

- (void)consumeCompleteLines {
    const uint8_t* bytes = static_cast<const uint8_t*>(_pendingBytes.bytes);
    NSUInteger lastNewline = NSNotFound;
    for (NSUInteger i = 0; i < _pendingBytes.length; ++i) {
        if (bytes[i] == '\n') lastNewline = i;
    }
    if (lastNewline == NSNotFound) return;

    NSData* complete = [_pendingBytes subdataWithRange:NSMakeRange(0, lastNewline + 1)];
    [_pendingBytes replaceBytesInRange:NSMakeRange(0, lastNewline + 1)
                             withBytes:nullptr
                                length:0];
    NSString* text = [[NSString alloc] initWithData:complete encoding:NSUTF8StringEncoding];
    if (!text) text = [[NSString alloc] initWithData:complete encoding:NSISOLatin1StringEncoding];
    if (!text.length) return;

    if (_discardLeadingFragment) {
        NSRange firstNewline = [text rangeOfString:@"\n"];
        text = firstNewline.location == NSNotFound
            ? @""
            : [text substringFromIndex:NSMaxRange(firstNewline)];
        _discardLeadingFragment = NO;
    }

    NSArray<NSString*>* lines = [text componentsSeparatedByString:@"\n"];
    for (NSUInteger i = 0; i + 1 < lines.count; ++i) {
        [self appendLogLine:[lines[i] stringByAppendingString:@"\n"]];
    }
    [self pruneIfNeeded];
    if (_autoScrollButton.state == NSControlStateValueOn) {
        [_textView scrollRangeToVisible:NSMakeRange(_textView.string.length, 0)];
    }
}

- (NSInteger)severityForLine:(NSString*)line {
    NSString* lower = line.lowercaseString;
    NSArray<NSString*>* errorTokens = @[
        @"[error]", @"error:", @"error=", @" exception", @"exception:",
        @"fatal", @"panic", @"assertion failed", @"unhandledrejection",
        @" failed", @" fail:", @" err:", @"失败", @"异常", @"崩溃"
    ];
    for (NSString* token in errorTokens) {
        if ([lower containsString:token]) return 2;
    }
    NSArray<NSString*>* warningTokens = @[@"[warn]", @"warning", @"⚠️", @"警告"];
    for (NSString* token in warningTokens) {
        if ([lower containsString:token]) return 1;
    }
    return 0;
}

- (void)appendLogLine:(NSString*)line {
    NSInteger severity = [self severityForLine:line];
    NSColor* color = [NSColor colorWithWhite:0.84 alpha:1.0];
    NSFontWeight weight = NSFontWeightRegular;
    if (severity == 2) {
        color = [NSColor colorWithSRGBRed:1.0 green:0.25 blue:0.29 alpha:1.0];
        weight = NSFontWeightSemibold;
    } else if (severity == 1) {
        color = NSColor.systemOrangeColor;
        weight = NSFontWeightMedium;
    }
    NSDictionary* attributes = @{
        NSForegroundColorAttributeName: color,
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12 weight:weight],
    };
    [_textView.textStorage appendAttributedString:
        [[NSAttributedString alloc] initWithString:line attributes:attributes]];
}

- (void)appendSpecialLine:(NSString*)line {
    NSDictionary* attributes = @{
        NSForegroundColorAttributeName: NSColor.systemTealColor,
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12
                                                        weight:NSFontWeightMedium],
    };
    [_textView.textStorage appendAttributedString:
        [[NSAttributedString alloc] initWithString:line attributes:attributes]];
}

- (void)pruneIfNeeded {
    NSTextStorage* storage = _textView.textStorage;
    if (storage.length <= kMacLiveLogMaximumCharacters) return;
    NSUInteger proposed = storage.length - kMacLiveLogRetainedCharacters;
    NSRange search = NSMakeRange(MIN(proposed, storage.length),
                                 storage.length - MIN(proposed, storage.length));
    NSRange newline = [storage.string rangeOfString:@"\n" options:0 range:search];
    NSUInteger removeLength = newline.location == NSNotFound
        ? proposed
        : NSMaxRange(newline);
    [storage deleteCharactersInRange:NSMakeRange(0, removeLength)];
    [self appendSpecialLine:@"──────── 较早的窗口内容已裁剪；完整历史仍保留在日志文件中 ────────\n"];
}

@end
