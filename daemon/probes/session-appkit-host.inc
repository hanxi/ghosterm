/* P0 native event adapter, not the production Swift host. No terminal output
 * enters this class: the process I/O thread feeds the surface directly. */
@interface P0AppKitHost : NSView <NSTextInputClient>
@property(nonatomic,assign) ghostty_surface_t terminal;
@property(nonatomic,assign) BOOL inputEnabled;
@property(nonatomic,assign) unsigned blockedEvents,downs,ups,keys;
@property(nonatomic,copy) NSString *marked;
@property(nonatomic,assign) NSRange selection;
@property(nonatomic,assign) BOOL interpreting,handled;
@end
@implementation P0AppKitHost
- (BOOL)acceptsFirstResponder {return YES;}
- (BOOL)acceptsFirstMouse:(NSEvent *)event {(void)event;return YES;}
- (void)syncPreedit {
    const char *bytes=self.marked.UTF8String;
    ghostty_surface_preedit(_terminal,bytes,bytes?strlen(bytes):0);
}
- (void)keyDown:(NSEvent *)event {
    if(!_inputEnabled || !_terminal){_blockedEvents++;return;}
    _keys++;_interpreting=YES;_handled=NO;BOOL wasMarked=self.hasMarkedText;
    [self interpretKeyEvents:@[event]];_interpreting=NO;[self syncPreedit];
    if(_handled || wasMarked || self.hasMarkedText)return;
    ghostty_input_mods_e mods=GHOSTTY_MODS_NONE;
    if(event.modifierFlags & NSEventModifierFlagShift)mods|=GHOSTTY_MODS_SHIFT;
    if(event.modifierFlags & NSEventModifierFlagControl)mods|=GHOSTTY_MODS_CTRL;
    if(event.modifierFlags & NSEventModifierFlagOption)mods|=GHOSTTY_MODS_ALT;
    if(event.modifierFlags & NSEventModifierFlagCommand)mods|=GHOSTTY_MODS_SUPER;
    ghostty_input_key_s key={.action=GHOSTTY_ACTION_PRESS,.mods=mods,.keycode=event.keyCode,.text=event.characters.UTF8String};
    ghostty_surface_key(_terminal,key);
}
- (void)doCommandBySelector:(SEL)selector {(void)selector;/* keyDown fallback encodes non-text keys. */}
- (BOOL)hasMarkedText {return self.marked.length>0;}
- (NSRange)markedRange {return self.hasMarkedText?NSMakeRange(0,self.marked.length):NSMakeRange(NSNotFound,0);}
- (NSRange)selectedRange {return self.hasMarkedText?_selection:NSMakeRange(0,0);}
- (void)setMarkedText:(id)value selectedRange:(NSRange)selection replacementRange:(NSRange)replacement {
    (void)replacement;if(!_inputEnabled || !_terminal){_blockedEvents++;return;}
    NSString *text=[value isKindOfClass:NSAttributedString.class]?[value string]:value;
    CHECK([text isKindOfClass:NSString.class]);self.marked=text;
    CHECK(selection.location<=text.length && selection.length<=text.length-selection.location);_selection=selection;
    _handled=YES;if(!_interpreting)[self syncPreedit];
}
- (void)unmarkText {self.marked=@"";_selection=NSMakeRange(0,0);if(_terminal && _inputEnabled)[self syncPreedit];}
- (void)insertText:(id)value replacementRange:(NSRange)replacement {
    (void)replacement;if(!_inputEnabled || !_terminal){_blockedEvents++;return;}
    NSString *text=[value isKindOfClass:NSAttributedString.class]?[value string]:value;
    CHECK([text isKindOfClass:NSString.class]);[self unmarkText];_handled=YES;
    ghostty_input_key_s key={.action=GHOSTTY_ACTION_PRESS,.mods=GHOSTTY_MODS_NONE,.text=text.UTF8String};
    ghostty_surface_key(_terminal,key);
}
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText {return @[];}
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual {
    (void)range;if(actual)*actual=NSMakeRange(NSNotFound,0);return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {(void)point;return NSNotFound;}
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual {
    if(actual)*actual=range;if(!_terminal)return NSZeroRect;
    double x=0,y=0,w=0,h=0;ghostty_surface_ime_point(_terminal,&x,&y,&w,&h);
    return [self.window convertRectToScreen:[self convertRect:NSMakeRect(x,NSHeight(self.bounds)-y,w,h) toView:nil]];
}
- (void)sendPosition:(NSEvent *)event {
    NSPoint point=[self convertPoint:event.locationInWindow fromView:nil];
    ghostty_surface_mouse_pos(_terminal,point.x,NSHeight(self.bounds)-point.y,GHOSTTY_MODS_NONE);
}
- (void)mouseDown:(NSEvent *)event {
    if(!_inputEnabled || !_terminal){_blockedEvents++;return;}
    [self.window makeFirstResponder:self];[self sendPosition:event];_downs++;
    ghostty_surface_mouse_button(_terminal,GHOSTTY_MOUSE_PRESS,GHOSTTY_MOUSE_LEFT,GHOSTTY_MODS_NONE);
}
- (void)mouseUp:(NSEvent *)event {
    if(!_inputEnabled || !_terminal){_blockedEvents++;return;}
    [self sendPosition:event];_ups++;
    ghostty_surface_mouse_button(_terminal,GHOSTTY_MOUSE_RELEASE,GHOSTTY_MOUSE_LEFT,GHOSTTY_MODS_NONE);
}
@end
