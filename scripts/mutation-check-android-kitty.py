#!/usr/bin/env python3
"""Kitty graphics: actual owner/Canvas assertions with same-APK Core control."""
import hashlib
import os
import re
import signal
MARK='GT_ANDROID_KITTY_MUTATION'
def execute(root,build,test):
    surface=root/'terminal/android/native/surface.c'
    owner=root/'android/bridge/src/main/cpp/session_jni.c'
    view=root/'terminal/android/src/main/java/com/hanxi/ghosterm/terminal/TerminalView.kt'
    png=root/'terminal/android/native/android_png.c'
    graphics=root/'terminal/android/native/android_graphics.c'
    limits=root/'patches/libghostty/android-vt-8159838/0005-kitty-android-limits.patch'
    virtual=root/'patches/libghostty/android-vt-8159838/0004-kitty-virtual-placements.patch'
    originals={p:p.read_bytes() for p in (surface,owner,view,png,graphics,limits,virtual)}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual Kitty mutation')
    canvas='com.hanxi.ghosterm.KittyCanvasTest#'
    engine='com.hanxi.ghosterm.KittyAnimationEngineTest#'
    cases=[
      ('placement-bounds',limits,'d.columns > 65535 or d.rows > 65535','false',canvas+'excessivePlacementDimensionsAreRejectedWithoutDisconnectingTheTerminal'),
      ('relative',virtual,'+        if (!visit(context, entry.key_ptr.image_id,','+        if (false and !visit(context, entry.key_ptr.image_id,',canvas+'relativeChildUsesItsVirtualParentAndDisappearsWithoutVisiblePlaceholders'),
      ('clock',owner,'if(visible && (animation_revision!=s->revision || now>=animation_at))','if(false && (animation_revision!=s->revision || now>=animation_at))',canvas+'animationRepaintsWithoutNewOutputOrRebuildingTextSnapshots'),
      ('hidden-clock',owner,'if(visible && (animation_revision!=s->revision || now>=animation_at))','if(animation_revision!=s->revision || now>=animation_at)',canvas+'hiddenViewStopsAnimationClockAndResumesTheSameImage'),
      ('canvas',view,'currentGraphics?.draw(canvas,3,imagePaint)','Unit',canvas+'actualViewPaintsImageAndClearsDeletedPlacement'),
      ('background-layer',surface,'((style.bg_color.tag==GHOSTTY_STYLE_COLOR_NONE && !style.inverse) << 9)','(0 << 9)',canvas+'belowBackgroundImageRespectsExplicitBackgroundEvenWhenItMatchesDefault'),
      ('png-alpha',png,'(*env)->SetBooleanField(env,options,codec.premultiplied,JNI_FALSE);','(*env)->SetBooleanField(env,options,codec.premultiplied,JNI_TRUE);',engine+'platformPngDecoderPreservesStraightAlpha'),
      ('virtual',graphics,'if(ghostty_kitty_graphics_virtual_render(terminal,cw,ch,virtual_placement,&list)!=GHOSTTY_SUCCESS)goto done;','if(false && ghostty_kitty_graphics_virtual_render(terminal,cw,ch,virtual_placement,&list)!=GHOSTTY_SUCCESS)goto done;',canvas+'unicodePlaceholderUsesUpstreamGeometryInsteadOfPaintingATextGlyph'),
      ('dimensions',limits,'4096 else 10000','8192 else 10000',engine+'excessiveDimensionsAreRejectedAndAValidImageStillLoads'),
      ('frames',limits,'anim.frameCount() >= 1024','anim.frameCount() >= 2048',engine+'frameMetadataLimitRejectsAnExtraFrameAndLeavesPlaybackUsable'),
    ]
    wanted=set(os.environ['ANDROID_KITTY_CASES'].split(',')) if os.environ.get('ANDROID_KITTY_CASES') else None
    if wanted is not None and wanted-{c[0] for c in cases}:raise RuntimeError('Unknown Kitty case')
    def one(label,method,green):
        output=test(label,method,green)
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output or (not green and not re.search(r'Tests run: 1,\s+Failures: 1',output)):
            print(output,flush=True);raise RuntimeError('Expected one named Kitty assertion')
    def restore():
        for p,data in originals.items():
            if p.read_bytes()!=data:p.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=hashlib.sha256(data).digest() for p,data in originals.items()):raise RuntimeError('Kitty restore SHA256 mismatch')
    def interrupted(number,frame):raise KeyboardInterrupt('Kitty mutation interrupted')
    handlers={number:signal.signal(number,interrupted) for number in (signal.SIGINT,signal.SIGTERM)}
    count=0
    try:
        for name,path,old,new,method in cases:
            if wanted is not None and name not in wanted:continue
            clean=originals[path].decode()
            if clean.count(old)!=1:raise RuntimeError('Missing/ambiguous Kitty anchor: '+name)
            suffix='\n# '+MARK+'\n' if path.suffix=='.patch' else '\n// '+MARK+'\n'
            path.write_text(clean.replace(old,new)+suffix)
            label='kitty-'+name
            build(label);one(label,method,False)
            one(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            restore();build(label+'-restored');one(label+'-restored',method,True)
            count+=1;print('PASS '+label+': compiled assertion red / same APK Core green / restored green / SHA256',flush=True)
    finally:
        restore()
        for number,handler in handlers.items():signal.signal(number,handler)
    print(f'ANDROID KITTY MUTATIONS PASS: {count} groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_KITTY_MUTATE=1 with test-android-ssh.sh')
