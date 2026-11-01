package com.hanxi.ghosterm.terminal

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect

/** Immutable native-owner-created bitmaps; no terminal bytes, handles or decoder
 * work enter the View. Old frames keep their own Java references until GC. */
class TerminalGraphics private constructor(val revision: Long, private val records: IntArray,
    private val images: List<Bitmap>) {
    val placements get()=(records.size-4)/10
    fun draw(canvas: Canvas, layer: Int, paint: Paint) {
        for(i in 0 until placements) {
            val at=4+i*10
            val z=records[at+9]
            val actual=if(z<Int.MIN_VALUE/2) 1 else if(z<0) 2 else 3
            if(actual!=layer)continue
            val bitmap=images[records[at]]
            val source=Rect(records[at+1],records[at+2],records[at+1]+records[at+3],records[at+2]+records[at+4])
            val target=Rect(records[at+5],records[at+6],records[at+7],records[at+8])
            canvas.drawBitmap(bitmap,source,target,paint)
        }
    }
    companion object {
        fun decode(packet: Array<Any>): TerminalGraphics {
            require(packet.size==2)
            val records=packet[0] as IntArray
            val images=(packet[1] as Array<*>).map { it as Bitmap }
            require(records.size>=4 && records[2]==1 && records[3] in 0..69632 && records.size==4+records[3]*10)
            repeat(records[3]) { i ->
                val at=4+i*10
                require(records[at] in images.indices)
                val bitmap=images[records[at]]
                require(records[at+1]>=0 && records[at+2]>=0 && records[at+3]>=0 && records[at+4]>=0 &&
                    records[at+1].toLong()+records[at+3]<=bitmap.width && records[at+2].toLong()+records[at+4]<=bitmap.height)
            }
            return TerminalGraphics((records[0].toLong() and 0xffffffffL) or (records[1].toLong() shl 32),records,images)
        }
    }
}
