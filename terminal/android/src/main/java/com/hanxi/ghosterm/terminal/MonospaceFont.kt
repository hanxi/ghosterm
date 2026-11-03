package com.hanxi.ghosterm.terminal

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Validate the font's own ASCII cmap/hmtx, so system fallback cannot make a symbol font look monospaced. */
internal object MonospaceFont {
    fun validate(bytes:ByteArray) {
        require(bytes.size in 12..TerminalFontStore.MAX_FONT_BYTES) { "字体文件为空或超过 4 MiB" }
        val b=ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        fun u16(p:Int):Int { require(p>=0 && p<=bytes.size-2);return b.getShort(p).toInt() and 65535 }
        fun u32(p:Int):Long { require(p>=0 && p<=bytes.size-4);return b.getInt(p).toLong() and 0xffffffffL }
        require(u32(0) in listOf(0x00010000L,0x4f54544fL)) { "仅支持单字体 TTF / OTF 文件" }
        val n=u16(4);require(n in 1..128 && 12+n*16<=bytes.size)
        val tables=mutableMapOf<String,IntRange>()
        repeat(n) { i ->
            val p=12+i*16;val tag=bytes.copyOfRange(p,p+4).toString(Charsets.US_ASCII)
            val start=u32(p+8);val size=u32(p+12)
            require(start<=bytes.size && size<=bytes.size-start && tag !in tables)
            tables[tag]=start.toInt() until (start+size).toInt()
        }
        fun table(name:String,min:Int):IntRange=checkNotNull(tables[name]) { "字体缺少 $name 表" }.also { require(it.count()>=min) }
        val maxp=table("maxp",6);val glyphs=u16(maxp.first+4);require(glyphs>0)
        val hhea=table("hhea",36);val metrics=u16(hhea.first+34);require(metrics in 1..glyphs)
        val hmtx=table("hmtx",metrics*4+(glyphs-metrics)*2)
        val cmap=table("cmap",4);val records=u16(cmap.first+2);require(records in 1..128 && 4+records*8<=cmap.count())
        val subtables=(0 until records).mapNotNull { i ->
            val p=cmap.first+4+i*8;val platform=u16(p);val encoding=u16(p+2)
            if(platform!=0 && !(platform==3 && encoding in setOf(1,10))) return@mapNotNull null
            val offset=u32(p+4);require(offset<=cmap.count()-2)
            val sub=cmap.first+offset.toInt();val format=u16(sub)
            if(format !in setOf(4,12)) return@mapNotNull null
            val length=if(format==4) u16(sub+2).toLong() else u32(sub+4)
            require(length>=if(format==4) 16 else 16);require(length<=cmap.last-sub+1)
            sub until sub+length.toInt()
        }.sortedByDescending { u16(it.first) }
        require(subtables.isNotEmpty()) { "字体没有可用的 Unicode 字符表" }
        fun glyph(sub:IntRange,cp:Int):Int {
            val p=sub.first
            if(u16(p)==12) {
                val groups=u32(p+12);require(groups<=(sub.count()-16)/12)
                for(i in 0 until groups.toInt()) {
                    val q=p+16+i*12;val start=u32(q);val end=u32(q+4);require(start<=end)
                    if(cp.toLong() in start..end) return (u32(q+8)+cp-start).also { require(it<glyphs) }.toInt()
                }
                return 0
            }
            val seg=u16(p+6)/2;require(seg>0 && 16+8*seg<=sub.count())
            for(i in 0 until seg) {
                val end=u16(p+14+2*i);val start=u16(p+16+2*seg+2*i)
                if(cp !in start..end) continue
                val delta=u16(p+16+4*seg+2*i);val word=p+16+6*seg+2*i;val offset=u16(word)
                if(offset==0) return (cp+delta) and 65535
                val address=word+offset+2*(cp-start);require(address>=sub.first && address<=sub.last-1)
                val value=u16(address);return if(value==0) 0 else (value+delta) and 65535
            }
            return 0
        }
        val advances=(32..126).map { cp ->
            val index=subtables.firstNotNullOfOrNull { sub -> glyph(sub,cp).takeIf { it!=0 } }
                ?: error("字体必须包含完整的 ASCII 正文字符")
            require(index in 1 until glyphs)
            u16(hmtx.first+4*index.coerceAtMost(metrics-1))
        }
        require(advances.first()>0 && advances.all { it==advances.first() }) { "请选择等宽字体；该字体的字符宽度不同" }
    }
}
